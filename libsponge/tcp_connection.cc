#include "tcp_connection.hh"

#include <iostream>

// Dummy implementation of a TCP connection

// For Lab 4, please replace with a real implementation that passes the
// automated checks run by `make check`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

void TCPConnection::send_RST() {
    _sender.stream_in().set_error();
    _receiver.stream_out().set_error();
    _linger_after_streams_finish = false;
    while (!_sender.segments_out().empty()) {
        _sender.segments_out().pop();
    }
    _sender.send_empty_segment();
    TCPSegment seg = _sender.segments_out().front();
    _sender.segments_out().pop();
    seg.header().rst = true;
    _segments_out.push(seg);
}

void TCPConnection::send_all_segments_with_ack_and_window() {
    if (_closed) {
        return;
    }
    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();
        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
        }
        seg.header().win = min(_receiver.window_size(), static_cast<size_t>(numeric_limits<uint16_t>().max()));
        _segments_out.push(seg);
    }
}

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }

size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }

size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }

size_t TCPConnection::time_since_last_segment_received() const { return _time_since_last_segment_received; }

void TCPConnection::segment_received(const TCPSegment &seg) {
    if (!_syn_sent && !seg.header().syn) {
        return;
    }
    if (seg.header().rst) {
        _sender.stream_in().set_error();
        _receiver.stream_out().set_error();
        _linger_after_streams_finish = false;
    }

    // 接收 segment
    _time_since_last_segment_received = 0;
    _receiver.segment_received(seg);
    _sender.ack_received(seg.header().ackno, seg.header().win);
    // 检查是否是第 1 次挥手
    if (_receiver.stream_out().input_ended() && !_sender.stream_in().eof()) {
        // 收完了对方的数据，但是没有发完自己的数据，成为四次挥手的被动方
        _linger_after_streams_finish = false;
    }
    // 检查是否是第 3 次挥手
    if (_receiver.stream_out().input_ended() && _sender.stream_in().eof() && _sender.bytes_in_flight() == 0) {
        if (_linger_after_streams_finish) {
            // 满足了 3 个条件，作为 4 次挥手的主动方需要 time_wait
            _time_wait = true;
        }
    }

    // 回复 segment
    _sender.fill_window();
    _syn_sent = true;  // 第一次回复必定发送了 SYN
    // 如果当前没有数据或者没有 SYN 发送给对方
    // 如果接收到的不是 empty segment 回复一个 empty segment，否则不回复
    if (_sender.segments_out().empty() && seg.length_in_sequence_space() != 0) {
        _sender.send_empty_segment();
    }
    send_all_segments_with_ack_and_window();
}

bool TCPConnection::active() const {
    if (_sender.stream_in().error() && _receiver.stream_out().error()) {
        return false;
    }
    return !(_receiver.stream_out().input_ended() && _sender.stream_in().eof() && _sender.bytes_in_flight() == 0) ||
           _time_wait;
}

size_t TCPConnection::write(const string &data) {
    size_t write_bytes = _sender.stream_in().write(data);
    _sender.fill_window();
    send_all_segments_with_ack_and_window();
    return write_bytes;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) {
    _time_since_last_segment_received += ms_since_last_tick;
    if (_time_wait && _time_since_last_segment_received >= _cfg.rt_timeout * 10) {
        // closed
        _time_wait = false;
        _closed = true;
        return;
    }
    _sender.tick(ms_since_last_tick);
    if (_sender.consecutive_retransmissions() > _cfg.MAX_RETX_ATTEMPTS) {
        send_RST();
    } else {
        send_all_segments_with_ack_and_window();
    }
}

void TCPConnection::end_input_stream() {
    _sender.stream_in().end_input();
    _sender.fill_window();
    send_all_segments_with_ack_and_window();
}

void TCPConnection::connect() {
    if (!_syn_sent) {
        _sender.fill_window();
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();
        seg.header().win = 1;
        _segments_out.push(seg);
        _syn_sent = true;
    }
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";
            send_RST();
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}
