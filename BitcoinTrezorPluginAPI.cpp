/**********************************************************\

  Auto-generated BitcoinTrezorPluginAPI.cpp

\**********************************************************/

#include "JSObject.h"
#include "global/config.h"

#include "BitcoinTrezorPluginAPI.h"
#include "exceptions.h"
#include "config.pb.h"

///////////////////////////////////////////////////////////////////////////////
/// @fn BitcoinTrezorPluginPtr BitcoinTrezorPluginAPI::getPlugin()
///
/// @brief  Gets a reference to the plugin that was passed in when the object
///         was created.  If the plugin has already been released then this
///         will throw a FB::script_error that will be translated into a
///         javascript exception in the page.
///////////////////////////////////////////////////////////////////////////////
BitcoinTrezorPluginPtr BitcoinTrezorPluginAPI::getPlugin()
{
    BitcoinTrezorPluginPtr plugin(m_plugin.lock());
    if (!plugin)
        throw FB::script_error("The plugin is invalid");
    return plugin;
}

/// Returns plugin version.
/// Unauthenticated.
std::string BitcoinTrezorPluginAPI::get_version()
{
    return FBSTRING_PLUGIN_VERSION;
}

/// Loads configuration from a serialized and signed Protobuf
/// Configuration message.
/// Unauthenticated.
void BitcoinTrezorPluginAPI::configure(const std::string &config_str_hex)
{
    try {
        const std::string config_str = utils::hex_decode(config_str_hex);
        const uint8_t *config_buf = (const uint8_t *)config_str.c_str();
        const size_t config_len = config_str.size();
        bool verified = false;

        if (config_len >= utils::SIGNATURE_LENGTH)
            verified = utils::signature_verify(config_buf,
                                               config_buf + utils::SIGNATURE_LENGTH,
                                               config_len - utils::SIGNATURE_LENGTH);
        if (!verified) {
            FBLOG_ERROR("configure()", "Signature verification failed");
            throw ConfigurationError("Signature verification failed");
        }

        Configuration config;
        config.ParseFromArray(config_buf + utils::SIGNATURE_LENGTH,
                              config_len - utils::SIGNATURE_LENGTH);
        getPlugin()->configure(config);

    } catch (const std::exception &e) {
        FBLOG_ERROR("configure()", "Exception caught");
        FBLOG_ERROR("configure()", e.what());
        throw FB::script_error(e.what());
    }
}

/// Lists allowed device objects.
/// Authentication required.
/// Returns array of BitcoinTrezorDeviceAPI.
std::vector<FB::JSAPIPtr> BitcoinTrezorPluginAPI::get_devices()
{
    if (!getPlugin()->authenticate()) {
        FBLOG_ERROR("get_devices()", "URL not allowed");
        throw ConfigurationError("URL not allowed");
    }

    try {
        std::vector<DeviceDescriptor> devices = getPlugin()->enumerate();
        std::vector<FB::JSAPIPtr> result;

        for (size_t i = 0; i < devices.size(); i++)
            result.push_back(boost::make_shared<BitcoinTrezorDeviceAPI>(devices[i]));

        return result;
        
    } catch (const std::exception &e) {
        FBLOG_ERROR("get_devices()", "Exception caught");
        FBLOG_ERROR("get_devices()", e.what());
        throw FB::script_error(e.what());
    }
}

/// Starts device communication thread.
void BitcoinTrezorDeviceAPI::open()
{
    try {
        FBLOG_INFO("open()", "Starting call consumer");
        if (_call_thread.joinable())
            throw std::logic_error("Already open");
        _call_queue.open();
        _call_thread = boost::thread(boost::bind(
            &BitcoinTrezorDeviceAPI::consume_calls, this));

    } catch (const std::exception &e) {
        FBLOG_ERROR("open()", "Exception caught");
        FBLOG_ERROR("open()", e.what());
        throw FB::script_error(e.what());
    }
}

/// Stops device communication thread.
void BitcoinTrezorDeviceAPI::close()
{
    try {
        FBLOG_INFO("close()", "Closing call queue, waiting for consumer to join");
        _call_queue.close();
        _call_thread.join();

    } catch (const std::exception &e) {
        FBLOG_ERROR("close()", "Exception caught");
        FBLOG_ERROR("close()", e.what());
        throw FB::script_error(e.what());
    }
}

/// Puts a device call to the call queue, to be picked up by the call
/// thread.
/// Errors returned through the callback param.
void BitcoinTrezorDeviceAPI::call(const std::string &type_name,
                                  const FB::VariantMap &message_map,
                                  const FB::JSObjectPtr &callback)
{
    try {
        FBLOG_INFO("call()", "Enqueing call job");
       _call_queue.put(DeviceCallJob(type_name, message_map, callback));

    } catch (const std::exception &e) {
        FBLOG_ERROR("call()", "Exception caught");
        FBLOG_ERROR("call()", e.what());
        callback->InvokeAsync("", FB::variant_list_of(e.what()));
    }
}

/// Device call job consumer. Runs in a special thread.
/// On error, closes the call queue with the exception.
void BitcoinTrezorDeviceAPI::consume_calls()
{
    FBLOG_INFO("consume_calls()", "Call job consumer started");

    try {
        HIDBuffer buffer;
        DeviceChannel channel(_device, &buffer);
        DeviceCallJob job;

        // wait for jobs and process them. process_call does not
        // throw, it passes the errors through the callback param
        while (_call_queue.get(job))
            process_call(channel, job.type_name, job.message_map, job.callback);

    } catch (const std::exception &e) {
        FBLOG_ERROR("consume_calls()", "Exception caught, closing");
        FBLOG_ERROR("consume_calls()", e.what());
        _call_queue.close(std::auto_ptr<std::exception>(
                              new std::exception(e))); // re-throw on next put
    }

    FBLOG_INFO("consume_calls()", "Call job consumer finished");
}

/// Executes an individual device call. Runs in a special thread.
/// Errors returned through the callback param.
void BitcoinTrezorDeviceAPI::process_call(DeviceChannel &channel,
                                          const std::string &type_name,
                                          const FB::VariantMap &message_map,
                                          const FB::JSObjectPtr &callback)
{
    try {
        FBLOG_INFO("process_call()", "Call starting");

        std::auto_ptr<PB::Message> outmsg;
        std::auto_ptr<PB::Message> inmsg = create_message(type_name);
        message_from_map(*inmsg, message_map);

        channel.write(*inmsg);
        outmsg = channel.read();

        FBLOG_INFO("process_call()", "Call finished");

        callback->InvokeAsync("", FB::variant_list_of
                              (false)
                              (message_name(*outmsg))
                              (message_to_map(*outmsg)));

    } catch (const std::exception &e) {
        FBLOG_FATAL("process_call()", "Exception occurred");
        FBLOG_FATAL("process_call()", e.what());
        callback->InvokeAsync("", FB::variant_list_of(e.what()));
    }
}
