#include <Storages/DeltaMerge/DMContext.h>
#include <Storages/DeltaMerge/File/DMFileBlockInputStream.h>
#include <Storages/DeltaMerge/FilterHelper.h>
#include <Storages/DeltaMerge/StableValueSpace.h>
#include <Storages/DeltaMerge/StoragePool.h>
#include <Storages/DeltaMerge/WriteBatches.h>
#include <Storages/PathPool.h>

namespace DB
{
namespace DM
{

const Int64 StableValueSpace::CURRENT_VERSION = 1;

void StableValueSpace::setFiles(const DMFiles & files_, DMContext * dm_context, HandleRange range)
{
    UInt64 rows  = 0;
    UInt64 bytes = 0;

    if (range.all())
    {
        for (auto & file : files_)
        {
            rows += file->getRows();
            bytes += file->getBytes();
        }
    }
    else
    {
        auto index_cache = dm_context->db_context.getGlobalContext().getMinMaxIndexCache().get();
        auto hash_salt   = dm_context->hash_salt;
        for (auto & file : files_)
        {
            DMFilePackFilter pack_filter(file, index_cache, hash_salt, range, EMPTY_FILTER, {});
            auto [file_valid_rows, file_valid_bytes] = pack_filter.validRowsAndBytes();
            rows += file_valid_rows;
            bytes += file_valid_bytes;
        }
    }

    this->valid_rows  = rows;
    this->valid_bytes = bytes;
    this->files       = files_;
}

void StableValueSpace::saveMeta(WriteBatch & meta_wb)
{
    MemoryWriteBuffer buf(0, 8192);
    writeIntBinary(CURRENT_VERSION, buf);
    writeIntBinary(valid_rows, buf);
    writeIntBinary(valid_bytes, buf);
    writeIntBinary((UInt64)files.size(), buf);
    for (auto & f : files)
        writeIntBinary(f->refId(), buf);

    auto data_size = buf.count(); // Must be called before tryGetReadBuffer.
    meta_wb.putPage(id, 0, buf.tryGetReadBuffer(), data_size);
}

StableValueSpacePtr StableValueSpace::restore(DMContext & context, PageId id)
{
    auto stable = std::make_shared<StableValueSpace>(id);

    Page                 page = context.storage_pool.meta().read(id);
    ReadBufferFromMemory buf(page.data.begin(), page.data.size());
    UInt64               version, valid_rows, valid_bytes, size;
    readIntBinary(version, buf);
    if (version != CURRENT_VERSION)
        throw Exception("Unexpected version: " + DB::toString(version));

    readIntBinary(valid_rows, buf);
    readIntBinary(valid_bytes, buf);
    readIntBinary(size, buf);
    UInt64 ref_id;
    for (size_t i = 0; i < size; ++i)
    {
        readIntBinary(ref_id, buf);

        auto file_id          = context.storage_pool.data().getNormalPageId(ref_id);
        auto file_parent_path = context.extra_paths.getPath(file_id) + "/" + STABLE_FOLDER_NAME;

        auto dmfile = DMFile::restore(file_id, ref_id, file_parent_path);
        stable->files.push_back(dmfile);
    }

    stable->valid_rows  = valid_rows;
    stable->valid_bytes = valid_bytes;

    return stable;
}

SkippableBlockInputStreamPtr StableValueSpace::getInputStream(const DMContext &     context, //
                                                              const ColumnDefines & read_columns,
                                                              const HandleRange &   handle_range,
                                                              const RSOperatorPtr & filter,
                                                              UInt64                max_data_version,
                                                              bool                  enable_clean_read)
{
    LOG_DEBUG(log, __FUNCTION__ << "max_data_version: " << max_data_version << ", enable_clean_read: " << enable_clean_read);

    SkippableBlockInputStreams streams;
    for (auto & file : files)
    {
        streams.push_back(std::make_shared<DMFileBlockInputStream>( //
            context.db_context,
            max_data_version,
            enable_clean_read,
            context.hash_salt,
            file,
            read_columns,
            handle_range,
            filter,
            IdSetPtr{}));
    }
    return std::make_shared<ConcatSkippableBlockInputStream>(streams);
}

size_t StableValueSpace::getRows()
{
    return valid_rows;
}

size_t StableValueSpace::getBytes()
{
    return valid_bytes;
}

size_t StableValueSpace::getPacks()
{
    size_t packs = 0;
    for (auto & file : files)
        packs += file->getPacks();
    return packs;
}

String StableValueSpace::getDMFilesString()
{
    String s;
    for (auto & file : files)
        s += "dmf_" + DB::toString(file->fileId()) + ",";
    if (!s.empty())
        s.erase(s.length() - 1);
    return s;
}

void StableValueSpace::enableDMFilesGC()
{
    for (auto & file : files)
        file->enableGC();
}

void StableValueSpace::recordRemovePacksPages(WriteBatches & wbs) const
{
    for (auto & file : files)
    {
        // Here we should remove the ref id instead of file_id.
        // Because a dmfile could be used by several segments, and only after all ref_ids are removed, then the file_id removed.
        wbs.removed_data.delPage(file->refId());
    }
}


} // namespace DM
} // namespace DB