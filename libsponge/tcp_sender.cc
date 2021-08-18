#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <random>

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity) {}

uint64_t TCPSender::bytes_in_flight() const { return _next_seqno - _expect_ack; }

void TCPSender::fill_window() {
    if (_next_seqno == 0) {
        // TCP 连接未建立，发送带 SYN 的 segament
        TCPSegment seg;
        seg.header().syn = true;
        seg.header().seqno = wrap(0, _isn);
        _segments_out.push(seg);
        _segments_not_acked.push(seg);
        _next_seqno = 1;
        _retrans_timer = _tick + _initial_retransmission_timeout;  // 第一次开启 timer
        return;
    }
    if (_expect_ack != 0) {
        // TCP 连接已建立
        bool send = false;
        uint64_t bytes_in_wait = _window_size - bytes_in_flight();
        // 带 FIN 的 segament 可能带 payload 也可能不带 payload，先考虑简单的不带 payload 的情况
        // 需要 _fin_sent 保证只发送一次
        if (bytes_in_wait > 0 && _stream.eof() && !_fin_sent) {
            TCPSegment seg;
            seg.header().fin = true;
            seg.header().seqno = wrap(_next_seqno, _isn);
            _segments_out.push(seg);
            _segments_not_acked.push(seg);
            _next_seqno += 1;
            _fin_sent = true;
            send = true;
        } else {
            // 发送带 payload 的 segament，这个 segament 可能带有 FIN
            while (bytes_in_wait > 0 && _stream.buffer_size() > 0) {
                uint64_t send_bytes = min(bytes_in_wait, TCPConfig::MAX_PAYLOAD_SIZE);
                Buffer payload(_stream.read(send_bytes));
                TCPSegment seg;
                seg.header().seqno = wrap(_next_seqno, _isn);
                seg.payload() = payload;
                _next_seqno += seg.length_in_sequence_space();
                bytes_in_wait = _window_size - bytes_in_flight();
                if (bytes_in_wait > 0 && _stream.eof() && !_fin_sent) {
                    // 如果当前 segament 恰好遇到 _stream.eof() 并且之前没有发送带 FIN 的 segament，则顺带 FIN
                    seg.header().fin = true;
                    _next_seqno += 1;
                    _fin_sent = true;
                }
                _segments_out.push(seg);
                _segments_not_acked.push(seg);
                send = true;
            }
        }
        if (send && _retrans_timer == 0) {
            // 重新开启 timer
            _retrans_timer = _tick + _initial_retransmission_timeout;
        }
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    if (window_size == 0) {
        _window_size = 1;
        _zero_window = true;
    } else {
        _window_size = window_size;
        _zero_window = false;
    }
    uint64_t ack = unwrap(ackno, _isn, _expect_ack);
    if (ack > _expect_ack && ack <= _next_seqno) {
        _expect_ack = ack;
        // 移除所有被 ack 的 segaments
        while (!_segments_not_acked.empty()) {
            TCPSegment seg = _segments_not_acked.front();
            if (seg.length_in_sequence_space() + unwrap(seg.header().seqno, _isn, _expect_ack) > _expect_ack) {
                break;
            }
            _segments_not_acked.pop();
        }
        // 如果所有 in_fight 的 segaments 都被 ack 了，则关闭 timer，否则重启 timer
        _retrans_timer = ack == _next_seqno ? 0 : _tick + _initial_retransmission_timeout;
        _consecutive_retransmissions = 0;
        _rto_back_off = 0;
    }
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    _tick += ms_since_last_tick;

    if (_tick >= _retrans_timer && !_segments_not_acked.empty()) {
        // 超时重传 seq == _expect_ack 的 segment（earliest segament）
        _segments_out.push(_segments_not_acked.front());
        _consecutive_retransmissions += 1;
        _rto_back_off += _zero_window ? 0 : 1;
        _retrans_timer = _tick + (_initial_retransmission_timeout << _rto_back_off);
    }
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retransmissions; }

void TCPSender::send_empty_segment() {
    TCPSegment seg;
    seg.header().seqno = wrap(_next_seqno, _isn);
    _segments_out.push(seg);
}
