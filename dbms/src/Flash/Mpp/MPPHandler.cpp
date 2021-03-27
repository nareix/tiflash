#include <Common/FailPoint.h>
#include <Common/TiFlashMetrics.h>
#include <Flash/Coprocessor/DAGBlockOutputStream.h>
#include <Flash/Coprocessor/DAGCodec.h>
#include <Flash/Coprocessor/DAGUtils.h>
#include <Flash/Coprocessor/StreamingDAGResponseWriter.h>
#include <Flash/CoprocessorHandler.h>
#include <Flash/Mpp/MPPHandler.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/executeQuery.h>
#include <Storages/Transaction/TMTContext.h>

namespace DB
{

namespace FailPoints
{
extern const char hang_in_execution[];
extern const char exception_before_mpp_register_non_root_mpp_task[];
extern const char exception_before_mpp_register_root_mpp_task[];
extern const char exception_before_mpp_register_tunnel_for_non_root_mpp_task[];
extern const char exception_before_mpp_register_tunnel_for_root_mpp_task[];
extern const char exception_during_mpp_register_tunnel_for_non_root_mpp_task[];
extern const char exception_before_mpp_non_root_task_run[];
extern const char exception_before_mpp_root_task_run[];
extern const char exception_during_mpp_non_root_task_run[];
extern const char exception_during_mpp_root_task_run[];
} // namespace FailPoints

bool MPPTaskProgress::isTaskHanging(const Context & context)
{
    bool ret = false;
    auto current_progress_value = current_progress.load();
    if (current_progress_value != progress_on_last_check)
    {
        /// make some progress
        found_no_progress = false;
    }
    else
    {
        /// no progress
        if (!found_no_progress)
        {
            /// first time on no progress
            found_no_progress = true;
            epoch_when_found_no_progress = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
        }
        else
        {
            /// no progress for a while, check timeout
            auto no_progress_duration
                = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count() - epoch_when_found_no_progress;
            auto timeout_threshold = current_progress_value == 0 ? context.getSettingsRef().mpp_task_waiting_timeout
                                                                 : context.getSettingsRef().mpp_task_running_timeout;
            if (no_progress_duration > timeout_threshold)
                ret = true;
        }
    }
    progress_on_last_check = current_progress_value;
    return ret;
}

void MPPTask::unregisterTask()
{
    if (manager != nullptr)
    {
        LOG_DEBUG(log, "task unregistered");
        manager->unregisterTask(this);
    }
    else
    {
        LOG_ERROR(log, "task manager is unset");
    }
}

void MPPTask::prepare(const mpp::DispatchTaskRequest & task_request)
{
    auto start_time = Clock::now();
    dag_req = std::make_unique<tipb::DAGRequest>();
    if (!dag_req->ParseFromString(task_request.encoded_plan()))
    {
        throw TiFlashException(
            std::string(__PRETTY_FUNCTION__) + ": Invalid encoded plan: " + task_request.encoded_plan(), Errors::Coprocessor::BadRequest);
    }
    std::unordered_map<RegionID, RegionInfo> regions;
    for (auto & r : task_request.regions())
    {
        auto res = regions.emplace(r.region_id(),
            RegionInfo(r.region_id(), r.region_epoch().version(), r.region_epoch().conf_ver(),
                CoprocessorHandler::GenCopKeyRange(r.ranges()), nullptr));
        if (!res.second)
            throw TiFlashException(std::string(__PRETTY_FUNCTION__) + ": contain duplicate region " + std::to_string(r.region_id()),
                Errors::Coprocessor::BadRequest);
    }
    // set schema ver and start ts.
    auto schema_ver = task_request.schema_ver();
    auto start_ts = task_request.meta().start_ts();

    context.setSetting("read_tso", start_ts);
    context.setSetting("schema_version", schema_ver);
    if (unlikely(task_request.timeout() < 0))
    {
        /// this is only for test
        context.setSetting("mpp_task_timeout", (Int64)5);
        context.setSetting("mpp_task_running_timeout", (Int64)10);
    }
    else
    {
        context.setSetting("mpp_task_timeout", task_request.timeout());
        if (task_request.timeout() > 0)
        {
            /// in the implementation, mpp_task_timeout is actually the task writing tunnel timeout
            /// so make the mpp_task_running_timeout a little bigger than mpp_task_timeout
            context.setSetting("mpp_task_running_timeout", task_request.timeout() + 30);
        }
    }
    context.getTimezoneInfo().resetByDAGRequest(*dag_req);
    context.setProgressCallback([this](const Progress & progress) { this->updateProgress(progress); });

    dag_context = std::make_unique<DAGContext>(*dag_req, task_request.meta());
    context.setDAGContext(dag_context.get());

    // register task.
    TMTContext & tmt_context = context.getTMTContext();
    auto task_manager = tmt_context.getMPPTaskManager();
    LOG_DEBUG(log, "begin to register the task " << id.toString());

    if (dag_context->isRootMPPTask())
    {
        FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_before_mpp_register_root_mpp_task);
    }
    else
    {
        FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_before_mpp_register_non_root_mpp_task);
    }
    if (!task_manager->registerTask(shared_from_this()))
    {
        throw TiFlashException(std::string(__PRETTY_FUNCTION__) + ": Failed to register MPP Task", Errors::Coprocessor::BadRequest);
    }

    DAGQuerySource dag(context, regions, *dag_req, true);

    if (dag_context->isRootMPPTask())
    {
        FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_before_mpp_register_tunnel_for_root_mpp_task);
    }
    else
    {
        FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_before_mpp_register_tunnel_for_non_root_mpp_task);
    }
    // register tunnels
    MPPTunnelSetPtr tunnel_set = std::make_shared<MPPTunnelSet>();
    const auto & exchangeSender = dag_req->root_executor().exchange_sender();
    std::chrono::seconds timeout(task_request.timeout());
    for (int i = 0; i < exchangeSender.encoded_task_meta_size(); i++)
    {
        // exchange sender will register the tunnels and wait receiver to found a connection.
        mpp::TaskMeta task_meta;
        task_meta.ParseFromString(exchangeSender.encoded_task_meta(i));
        MPPTunnelPtr tunnel = std::make_shared<MPPTunnel>(task_meta, task_request.meta(), timeout);
        LOG_DEBUG(log, "begin to register the tunnel " << tunnel->tunnel_id);
        registerTunnel(MPPTaskId{task_meta.start_ts(), task_meta.task_id()}, tunnel);
        tunnel_set->tunnels.emplace_back(tunnel);
        if (!dag_context->isRootMPPTask())
        {
            FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_during_mpp_register_tunnel_for_non_root_mpp_task);
        }
    }
    // read index , this may take a long time.
    io = executeQuery(dag, context, false, QueryProcessingStage::Complete);

    // get partition column ids
    auto part_keys = exchangeSender.partition_keys();
    std::vector<Int64> partition_col_id;
    for (const auto & expr : part_keys)
    {
        assert(isColumnExpr(expr));
        auto column_index = decodeDAGInt64(expr.val());
        partition_col_id.emplace_back(column_index);
    }
    // construct writer
    std::unique_ptr<DAGResponseWriter> response_writer
        = std::make_unique<StreamingDAGResponseWriter<MPPTunnelSetPtr>>(tunnel_set, partition_col_id, exchangeSender.tp(),
            context.getSettings().dag_records_per_chunk, dag.getEncodeType(), dag.getResultFieldTypes(), *dag_context);
    io.out = std::make_shared<DAGBlockOutputStream>(io.in->getHeader(), std::move(response_writer));
    auto end_time = Clock::now();
    Int64 compile_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    dag_context->compile_time_ns = compile_time_ns;
}

String taskStatusToString(TaskStatus ts)
{
    switch (ts)
    {
        case INITIALIZING:
            return "initializing";
        case RUNNING:
            return "running";
        case FINISHED:
            return "finished";
        case CANCELLED:
            return "cancelled";
        default:
            return "unknown";
    }
}
void MPPTask::runImpl()
{
    auto current_status = static_cast<TaskStatus>(status.load());
    if (current_status != INITIALIZING)
    {
        LOG_WARNING(log, "task in " + taskStatusToString(current_status) + " state, skip running");
        return;
    }
    current_memory_tracker = memory_tracker;
    Stopwatch stopwatch;
    LOG_INFO(log, "task starts running");
    status = RUNNING;
    auto from = io.in;
    auto to = io.out;
    try
    {
        from->readPrefix();
        to->writePrefix();
        LOG_DEBUG(log, "begin read ");

        size_t count = 0;

        while (Block block = from->read())
        {
            count += block.rows();
            to->write(block);
            FAIL_POINT_PAUSE(FailPoints::hang_in_execution);
            if (dag_context->isRootMPPTask())
            {
                FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_during_mpp_root_task_run);
            }
            else
            {
                FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_during_mpp_non_root_task_run);
            }
        }

        /// For outputting additional information in some formats.
        if (IProfilingBlockInputStream * input = dynamic_cast<IProfilingBlockInputStream *>(from.get()))
        {
            if (input->getProfileInfo().hasAppliedLimit())
                to->setRowsBeforeLimit(input->getProfileInfo().getRowsBeforeLimit());

            to->setTotals(input->getTotals());
            to->setExtremes(input->getExtremes());
        }

        from->readSuffix();
        to->writeSuffix();

        finishWrite();

        LOG_DEBUG(log, "finish write with " + std::to_string(count) + " rows");
    }
    catch (Exception & e)
    {
        LOG_ERROR(log, "task running meets error " << e.displayText() << " Stack Trace : " << e.getStackTrace().toString());
        writeErrToAllTunnel(e.displayText());
    }
    catch (std::exception & e)
    {
        LOG_ERROR(log, "task running meets error " << e.what());
        writeErrToAllTunnel(e.what());
    }
    catch (...)
    {
        LOG_ERROR(log, "unrecovered error");
        writeErrToAllTunnel("unrecovered fatal error");
    }
    LOG_INFO(log, "task ends, time cost is " << std::to_string(stopwatch.elapsedMilliseconds()) << " ms.");
    auto process_info = context.getProcessListElement()->getInfo();
    auto peak_memory = process_info.peak_memory_usage > 0 ? process_info.peak_memory_usage : 0;
    GET_METRIC(context.getTiFlashMetrics(), tiflash_coprocessor_request_memory_usage, type_dispatch_mpp_task).Observe(peak_memory);
    unregisterTask();
    status = FINISHED;
}

bool MPPTask::isTaskHanging()
{
    if (status.load() == RUNNING)
        return task_progress.isTaskHanging(context);
    return false;
}

void MPPTask::cancel(const String & reason)
{
    auto current_status = status.load();
    if (current_status == FINISHED || current_status == CANCELLED)
        return;
    LOG_WARNING(log, "Begin cancel task: " + id.toString());
    /// step 1. cancel query streams
    status = CANCELLED;
    auto process_list_element = context.getProcessListElement();
    if (process_list_element != nullptr && !process_list_element->streamsAreReleased())
    {
        BlockInputStreamPtr input_stream;
        BlockOutputStreamPtr output_stream;
        if (process_list_element->tryGetQueryStreams(input_stream, output_stream))
        {
            IProfilingBlockInputStream * input_stream_casted;
            if (input_stream && (input_stream_casted = dynamic_cast<IProfilingBlockInputStream *>(input_stream.get())))
            {
                input_stream_casted->cancel(true);
            }
        }
    }
    /// step 2. write Error msg and close the tunnel.
    /// Here we use `closeAllTunnel` because currently, `cancel` is a query level cancel, which
    /// means if this mpp task is cancelled, all the mpp tasks belonging to the same query are
    /// cancelled at the same time, so there is no guarantee that the tunnel can be connected.
    closeAllTunnel(reason);
    LOG_WARNING(log, "Finish cancel task: " + id.toString());
}

void MPPHandler::handleError(MPPTaskPtr task, String error)
{
    try
    {
        if (task != nullptr)
        {
            task->closeAllTunnel(error);
            task->unregisterTask();
        }
    }
    catch (...)
    {
        tryLogCurrentException(log, "Fail to handle error and clean task");
    }
}
// execute is responsible for making plan , register tasks and tunnels and start the running thread.
grpc::Status MPPHandler::execute(Context & context, mpp::DispatchTaskResponse * response)
{
    MPPTaskPtr task = nullptr;
    try
    {
        Stopwatch stopwatch;
        task = std::make_shared<MPPTask>(task_request.meta(), context);
        task->prepare(task_request);
        if (task->dag_context->isRootMPPTask())
        {
            FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_before_mpp_root_task_run);
        }
        else
        {
            FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_before_mpp_non_root_task_run);
        }
        task->memory_tracker = current_memory_tracker;
        task->run();
        LOG_INFO(log, "processing dispatch is over; the time cost is " << std::to_string(stopwatch.elapsedMilliseconds()) << " ms");
    }
    catch (Exception & e)
    {
        LOG_ERROR(log, "dispatch task meet error : " << e.displayText());
        auto * err = response->mutable_error();
        err->set_msg(e.displayText());
        handleError(task, e.displayText());
    }
    catch (std::exception & e)
    {
        LOG_ERROR(log, "dispatch task meet error : " << e.what());
        auto * err = response->mutable_error();
        err->set_msg(e.what());
        handleError(task, e.what());
    }
    catch (...)
    {
        LOG_ERROR(log, "dispatch task meet fatal error");
        auto * err = response->mutable_error();
        err->set_msg("fatal error");
        handleError(task, "fatal error");
    }
    return grpc::Status::OK;
}

MPPTaskManager::MPPTaskManager(BackgroundProcessingPool & background_pool_)
    : log(&Logger::get("TaskManager")), background_pool(background_pool_)
{
    handle = background_pool.addTask(
        [&, this] {
            bool has_hanging_query = false;
            try
            {
                /// get a snapshot of current queries
                auto current_query = this->getCurrentQueries();
                for (auto query_id : current_query)
                {
                    /// get a snapshot of current tasks
                    auto current_tasks = this->getCurrentTasksForQuery(query_id);
                    bool has_hanging_task = false;
                    for (auto & task : current_tasks)
                    {
                        if (task->isTaskHanging())
                        {
                            has_hanging_task = true;
                            break;
                        }
                    }
                    if (has_hanging_task)
                    {
                        has_hanging_query = true;
                        this->cancelMPPQuery(query_id, "MPP Task canceled because it seems hangs");
                    }
                }
            }
            catch (const Exception & e)
            {
                LOG_ERROR(log, "MPPTaskMonitor failed by " << e.displayText() << " \n stack : " << e.getStackTrace().toString());
            }
            catch (const Poco::Exception & e)
            {
                LOG_ERROR(log, "MPPTaskMonitor failed by " << e.displayText());
            }
            catch (const std::exception & e)
            {
                LOG_ERROR(log, "MPPTaskMonitor failed by " << e.what());
            }
            return has_hanging_query;
        },
        false);
}

MPPTaskManager::~MPPTaskManager() { background_pool.removeTask(handle); }

} // namespace DB