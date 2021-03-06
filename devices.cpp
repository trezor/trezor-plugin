#include "devices.h"
#include "messages.h"
#include "exceptions.h"
#include "utils.h"

#include "logging.h"

#include <boost/thread.hpp>

#include <sstream>
#include <time.h>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

boost::mutex hidapi_mutex;

void
HIDBuffer::read(hid_device *dev, uint8_t *bytes, size_t length, bool timeout)
{
    const time_t start_time = time(0);
    
    FBLOG_INFO("read()", "Starting to read n bytes");
    FBLOG_INFO("read()", length);
    
    FBLOG_DEBUG("read()", "Start buffer length");
    FBLOG_DEBUG("read()", _buffer_length);

    // buffer up enough chunks
    while (_buffer_length < length) {
        uint8_t chunk[1 + 63]; // extra byte for report number
        hidapi_mutex.lock();
        const int res = hid_read_timeout(dev, chunk, sizeof(chunk), 10);
        hidapi_mutex.unlock();

        if (res > 0) {
            int chlen = std::min(res, int(chunk[0])+1);
            FBLOG_INFO("read()", "Buffering n bytes");
            FBLOG_INFO("read()", chlen-1);
            for (size_t i = 1; i < chlen; i++, _buffer_length++)
                _buffer[_buffer_length] = chunk[i];
        }
        
        else if (res == 0 && timeout) { // check timeout
            time_t curr_time = time(0);
            if ((curr_time - start_time) > _read_timeout) {
                FBLOG_WARN("read()", "Timed out");
                throw ReadTimeout();
            }
            // check thread termination
            boost::this_thread::interruption_point();
        }
        
        else if (res < 0) { // read returns -1 on error
            hidapi_mutex.lock();
            const wchar_t *err = hid_error(dev);
            hidapi_mutex.unlock();
            FBLOG_FATAL("read()", "Read error");
            FBLOG_FATAL("read()", err);
            throw ReadError(err ? utils::utf8_encode(err) : "Unknown read error");
        }
    }
    
    FBLOG_DEBUG("read()", "Buffer length after buffering chunks");
    FBLOG_DEBUG("read()", _buffer_length);
    
    // read from the buffer
    size_t i;
    for (i = 0; i < length; i++) // copy to the result
        bytes[i] = _buffer[i];
    for (; i < _buffer_length; i++) // shift the buffer
        _buffer[i - length] = _buffer[i];
    _buffer_length -= length;
    
    FBLOG_DEBUG("read()", "End buffer length");
    FBLOG_DEBUG("read()", _buffer_length);
}

void
HIDBuffer::write(hid_device *dev, const uint8_t *bytes, size_t length)
{
    FBLOG_INFO("write_bytes()", "Starting to write n bytes");
    FBLOG_INFO("write_bytes()", length);
    
    for (size_t i = 0; i < length; i += 63) {
        uint8_t chunk[1 + 63] = {0}; // extra byte for report number
        size_t ncopy = std::min(sizeof(chunk) - 1, length - i);

        chunk[0] = sizeof(chunk) - 1;
        memcpy(&chunk[1], &bytes[i], ncopy);

        FBLOG_DEBUG("write_bytes()", "Writing chunk");
        hidapi_mutex.lock();
        const int res = hid_write(dev, chunk, sizeof(chunk));
        hidapi_mutex.unlock();
        
        if (res < sizeof(chunk)) {
            hidapi_mutex.lock();
            const wchar_t *err = hid_error(dev);
            hidapi_mutex.unlock();
            FBLOG_FATAL("write_bytes()", "Write error");
            FBLOG_FATAL("write_bytes()", err);
            throw WriteError(err ? utils::utf8_encode(err) : "Unknown read error");
        }
    }
}

void
DeviceChannel::open(const std::string &path)
{
    const unsigned char uart[] = {0x41, 0x01};
    const unsigned char txrx[] = {0x43, 0x03};

    FBLOG_INFO("open()", "Opening device");
    hidapi_mutex.lock();
    _device = hid_open_path(path.c_str());
    hidapi_mutex.unlock();

    if (!_device) {
        FBLOG_FATAL("open()", "Failed to open device");
        throw OpenError();
    }

    FBLOG_INFO("open()", "Sending feature reports");
    hidapi_mutex.lock();
    hid_send_feature_report(_device, uart, 2); // enable UART
    hid_send_feature_report(_device, txrx, 2); // purge TX/RX FIFOs
    hidapi_mutex.unlock();
}

void
DeviceChannel::close()
{
    FBLOG_INFO("close()", "Closing device");
    hidapi_mutex.lock();
    hid_close(_device);
    hidapi_mutex.unlock();
}

std::auto_ptr<PB::Message>
DeviceChannel::read(bool timeout)
{
    uint16_t type;
    uint32_t length;
    read_header(&type, &length, timeout);
    
    FBLOG_INFO("read()", "Type and Length");
    FBLOG_INFO("read()", type);
    FBLOG_INFO("read()", length);
    
    // check length looks valid before allocating
    if (length > HIDBuffer::BUFFER_SIZE) {
        FBLOG_FATAL("read()", "Message length is too big, probably invalid");
        throw ReadError("Invalid data");
    }
    
    std::vector<unsigned char> msgbuf(length);
    _buffer->read(_device, msgbuf.data(), length, timeout);

    std::string name = message_name(type);
    std::auto_ptr<PB::Message> message = create_message(name);
    message->ParseFromArray(msgbuf.data(), length);
    
    return message;
}

void
DeviceChannel::write(const PB::Message &message)
{
    const uint16_t type = message_type(message);
    const size_t msgsize = message.ByteSize();
    const size_t bufsize = 2 + 2 + 4 + msgsize; // ## + type + length + message
    
    std::vector<unsigned char> buf(bufsize);
    buf[0] = '#';
    buf[1] = '#';
    
    const uint16_t type_ = htons(type);
    buf[3] = type_ >> 8;
    buf[2] = type_ & 0xFF;
    
    const uint32_t length_ = htonl(msgsize);
    buf[7] = length_ >> 24;
    buf[6] = (length_ >> 16) & 0xFF;
    buf[5] = (length_ >> 8) & 0xFF;
    buf[4] = length_ & 0xFF;
    
    if (msgsize)
        message.SerializeToArray(&buf[8], msgsize);
    _buffer->write(_device, buf.data(), bufsize);
}

void
DeviceChannel::read_header(uint16_t *type, uint32_t *length, bool timeout)
{
    unsigned char header[6];

    FBLOG_INFO("read_header()", "Starting to read header");

    _buffer->read(_device, header, 1, timeout);
    while (header[0] != '#') {
        FBLOG_WARN("read_header()", "Warning: Aligning to magic characters");
        _buffer->read(_device, header, 1, timeout);
    }

    _buffer->read(_device, header, 1, timeout);
    if (header[0] != '#') {
        FBLOG_FATAL("read_header()", "Second magic character is broken");
        throw ReadError("Failed to read header");
    }

    FBLOG_INFO("read_header()", "Reading type and length");
    _buffer->read(_device, header, 6, timeout);
    *type = ntohs((header[0] << 0) | (header[1] << 8));
    *length = ntohl((header[2] << 0) | (header[3] << 8) |
                    (header[4] << 16) | (header[5] << 24));
}
