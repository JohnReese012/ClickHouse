#include "LibraryBridgeHelper.h"

#include <IO/ReadHelpers.h>
#include <DataStreams/OneBlockInputStream.h>
#include <DataStreams/OwningBlockInputStream.h>
#include <DataStreams/formatBlock.h>
#include <Dictionaries/DictionarySourceHelpers.h>
#include <Processors/Formats/InputStreamFromInputFormat.h>
#include <IO/WriteBufferFromOStream.h>
#include <IO/WriteBufferFromString.h>
#include <Formats/FormatFactory.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Common/ShellCommand.h>
#include <common/logger_useful.h>
#include <common/range.h>
#include <Core/Field.h>
#include <Common/escapeForFileName.h>


namespace DB
{

LibraryBridgeHelper::LibraryBridgeHelper(
        ContextPtr context_,
        const Block & sample_block_,
        const Field & dictionary_id_,
        const LibraryInitData & library_data_)
    : IBridgeHelper(context_->getGlobalContext())
    , log(&Poco::Logger::get("LibraryBridgeHelper"))
    , sample_block(sample_block_)
    , config(context_->getConfigRef())
    , http_timeout(context_->getGlobalContext()->getSettingsRef().http_receive_timeout.value)
    , library_data(library_data_)
    , dictionary_id(dictionary_id_)
{
    bridge_port = config.getUInt("library_bridge.port", DEFAULT_PORT);
    bridge_host = config.getString("library_bridge.host", DEFAULT_HOST);
}


Poco::URI LibraryBridgeHelper::createRequestURI(const String & method) const
{
    auto uri = getMainURI();
    uri.addQueryParameter("dictionary_id", toString(dictionary_id));
    uri.addQueryParameter("method", method);
    return uri;
}


Poco::URI LibraryBridgeHelper::createBaseURI() const
{
    Poco::URI uri;
    uri.setHost(bridge_host);
    uri.setPort(bridge_port);
    uri.setScheme("http");
    return uri;
}


void LibraryBridgeHelper::startBridge(std::unique_ptr<ShellCommand> cmd) const
{
    getContext()->addBridgeCommand(std::move(cmd));
}


bool LibraryBridgeHelper::checkBridgeIsRunning() const
{
    String result;
    try
    {
        ReadWriteBufferFromHTTP buf(createRequestURI(PING), Poco::Net::HTTPRequest::HTTP_GET, {}, ConnectionTimeouts::getHTTPTimeouts(getContext()));
        readString(result, buf);
    }
    catch (...)
    {
        return false;
    }

    /*
     * When pinging bridge we also pass current dicionary_id. The bridge will check if there is such
     * dictionary. It is possible that such dictionary_id is not present only in two cases:
     * 1. It is dictionary source creation and initialization of library handler on bridge side did not happen yet.
     * 2. Bridge crashed or restarted for some reason while server did not.
    **/
    static constexpr auto dictionary_check = "dictionary=";
    if (result.size() != (std::strlen(dictionary_check) + 1))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected message from library bridge: {}. Check bridge and server have the same version.",
                        result, std::strlen(dictionary_check));

    UInt8 dictionary_id_exists;
    auto parsed = tryParse<UInt8>(dictionary_id_exists, result.substr(std::strlen(dictionary_check)));
    if (!parsed || (dictionary_id_exists != 0 && dictionary_id_exists != 1))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected message from library bridge: {} ({}). Check bridge and server have the same version.",
                        result, parsed ? toString(dictionary_id_exists) : "failed to parse");

    if (dictionary_id_exists && !library_initialized)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Library was not initialized, but bridge responded to already have dictionary id: {}", dictionary_id);

    if (!dictionary_id_exists && library_initialized)
    {
        LOG_WARNING(log, "Library bridge does not have library handler with dictionaty id: {}. It will be reinitialized.", dictionary_id);
        try
        {
            if (!initLibrary(false))
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                                "Failed to reinitialize library handler on bridge side for dictionary with id: {}", dictionary_id);
        }
        catch (...)
        {
            tryLogCurrentException(log);
            return false;
        }
    }

    return true;
}


ReadWriteBufferFromHTTP::OutStreamCallback LibraryBridgeHelper::getInitLibraryCallback() const
{
    /// Sample block must contain null values
    WriteBufferFromOwnString out;
    auto output_stream = getContext()->getOutputStream(LibraryBridgeHelper::DEFAULT_FORMAT, out, sample_block);
    formatBlock(output_stream, sample_block);
    auto block_string = out.str();

    return [block_string, this](std::ostream & os)
    {
        os << "library_path=" << escapeForFileName(library_data.library_path) << "&";
        os << "library_settings=" << escapeForFileName(library_data.library_settings) << "&";
        os << "attributes_names=" << escapeForFileName(library_data.dict_attributes) << "&";
        os << "sample_block=" << escapeForFileName(sample_block.getNamesAndTypesList().toString()) << "&";
        os << "null_values=" << escapeForFileName(block_string);
    };
}


bool LibraryBridgeHelper::initLibrary(bool check_bridge) const
{
    /// Do not check if we call initLibrary from checkBridgeSync.
    if (check_bridge)
        startBridgeSync();
    auto uri = createRequestURI(LIB_NEW_METHOD);
    return executeRequest(uri, getInitLibraryCallback());
}


bool LibraryBridgeHelper::cloneLibrary(const Field & other_dictionary_id)
{
    startBridgeSync();
    auto uri = createRequestURI(LIB_CLONE_METHOD);
    uri.addQueryParameter("from_dictionary_id", toString(other_dictionary_id));
    return executeRequest(uri, getInitLibraryCallback());
}


bool LibraryBridgeHelper::removeLibrary()
{
    /// Do not force bridge restart if it is not running in case of removeLibrary
    /// because in this case after restart it will not have this dictionaty id in memory anyway.
    if (checkBridgeIsRunning())
    {
        auto uri = createRequestURI(LIB_DELETE_METHOD);
        return executeRequest(uri);
    }
    return true;
}


bool LibraryBridgeHelper::isModified()
{
    startBridgeSync();
    auto uri = createRequestURI(IS_MODIFIED_METHOD);
    return executeRequest(uri);
}


bool LibraryBridgeHelper::supportsSelectiveLoad()
{
    startBridgeSync();
    auto uri = createRequestURI(SUPPORTS_SELECTIVE_LOAD_METHOD);
    return executeRequest(uri);
}


BlockInputStreamPtr LibraryBridgeHelper::loadAll()
{
    startBridgeSync();
    auto uri = createRequestURI(LOAD_ALL_METHOD);
    return loadBase(uri);
}


BlockInputStreamPtr LibraryBridgeHelper::loadIds(const std::string, const std::vector<uint64_t> ids)
{
    startBridgeSync();
    auto uri = createRequestURI(LOAD_IDS_METHOD);

    uri.addQueryParameter("ids_num", toString(ids.size()));
    String ids_str;
    for (const auto & id : ids)
    {
        if (!ids_str.empty())
            ids_str += '-';
        ids_str += toString(id);
    }

    uri.addQueryParameter("ids", ids_str);
    std::cerr << "\n\nLibraryBridgeHelper: " << toString(dictionary_id) << " , ids_num: " << ids.size() << " , ids: " << ids_str << std::endl << std::endl;
    LOG_ERROR(log, "dictionary_id: {}, ids_num: {}, ids: {}", dictionary_id, ids.size(), ids_str);

    return loadBase(uri, [ids_str](std::ostream & os) { os << ids_str; });
}


BlockInputStreamPtr LibraryBridgeHelper::loadKeys(const Block & requested_block)
{
    startBridgeSync();
    auto uri = createRequestURI(LOAD_KEYS_METHOD);
    /// Sample block to parse block from callback
    uri.addQueryParameter("requested_block_sample", requested_block.getNamesAndTypesList().toString());
    ReadWriteBufferFromHTTP::OutStreamCallback out_stream_callback = [requested_block, this](std::ostream & os)
    {
        WriteBufferFromOStream out_buffer(os);
        auto output_stream = getContext()->getOutputStream(LibraryBridgeHelper::DEFAULT_FORMAT, out_buffer, sample_block);
        formatBlock(output_stream, requested_block);
    };
    return loadBase(uri, out_stream_callback);
}


bool LibraryBridgeHelper::executeRequest(const Poco::URI & uri, ReadWriteBufferFromHTTP::OutStreamCallback out_stream_callback) const
{
    ReadWriteBufferFromHTTP buf(
        uri,
        Poco::Net::HTTPRequest::HTTP_POST,
        std::move(out_stream_callback),
        ConnectionTimeouts::getHTTPTimeouts(getContext()));

    bool res;
    readBoolText(res, buf);
    return res;
}


BlockInputStreamPtr LibraryBridgeHelper::loadBase(const Poco::URI & uri, ReadWriteBufferFromHTTP::OutStreamCallback out_stream_callback)
{
    auto read_buf_ptr = std::make_unique<ReadWriteBufferFromHTTP>(
        uri,
        Poco::Net::HTTPRequest::HTTP_POST,
        std::move(out_stream_callback),
        ConnectionTimeouts::getHTTPTimeouts(getContext()),
        0,
        Poco::Net::HTTPBasicCredentials{},
        DBMS_DEFAULT_BUFFER_SIZE,
        ReadWriteBufferFromHTTP::HTTPHeaderEntries{});

    auto input_stream = getContext()->getInputFormat(LibraryBridgeHelper::DEFAULT_FORMAT, *read_buf_ptr, sample_block, DEFAULT_BLOCK_SIZE);
    return std::make_shared<OwningBlockInputStream<ReadWriteBufferFromHTTP>>(input_stream, std::move(read_buf_ptr));
}

}
