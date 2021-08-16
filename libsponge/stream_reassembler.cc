#include "stream_reassembler.hh"

// Dummy implementation of a stream reassembler.

// For Lab 1, please replace with a real implementation that passes the
// automated checks run by `make check_lab1`.

// You will need to add private members to the class declaration in `stream_reassembler.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

StreamReassembler::StreamReassembler(const size_t capacity) : _output(capacity), _capacity(capacity) {}

int StreamReassembler::merge_block(block_node &elm1, const block_node &elm2) {
    block_node x, y;
    if (elm1.begin > elm2.begin) {
        x = elm2;
        y = elm1;
    } else {
        x = elm1;
        y = elm2;
    }
    if (x.begin + x.length < y.begin) {
        return -1;  // 不能拼接起来
    }
    if (x.begin + x.length >= y.begin + y.length) {
        elm1 = x;
        return y.length;
    }
    elm1.begin = x.begin;
    elm1.data = x.data + y.data.substr(x.begin + x.length - y.begin);
    elm1.length = elm1.data.length();
    return x.begin + x.length - y.begin;
}

//! \details This function accepts a substring (aka a segment) of bytes,
//! possibly out-of-order, from the logical stream, and assembles any newly
//! contiguous substrings and writes them into the output stream in order.
void StreamReassembler::push_substring(const string &data, const size_t index, const bool eof) {
    if (index >= _expect + _capacity) {  // data.length() 可能是 0，所以 index + data.length() < _expect 不能直接返回
        return;
    }
    if (index + data.length() > _expect) {  // 当前 segament 有用，需要处理
        block_node elm;
        size_t offset = _expect > index ? _expect - index : 0;
        elm.begin = index + offset;
        elm.data.assign(data.begin() + offset, data.end());
        elm.length = elm.data.length();
        _unassembled_bytes += elm.length;

        // 合并 segaments
        // 1. 合并 index 比当前 block 更大的 blocks
        int merged_bytes = 0;
        auto iter = _set.lower_bound(elm);
        while (iter != _set.end() && (merged_bytes = merge_block(elm, *iter)) >= 0) {
            _unassembled_bytes -= merged_bytes;
            _set.erase(iter);
            iter = _set.lower_bound(elm);
        }
        // 2. 合并 index 比当前 block 更小的 blocks
        while (iter != _set.begin()) {
            iter--;  // iter 指向下一个比 ele 小的 block
            if ((merged_bytes = merge_block(elm, *iter)) < 0) {
                break;
            }
            _unassembled_bytes -= merged_bytes;
            _set.erase(iter);
            iter = _set.lower_bound(elm);
        }
        _set.insert(elm);

        // 如果 index 最小的 block 恰好是需要的，则将 index 最小的 block 写入
        if (_set.begin()->begin == _expect) {
            size_t write_bytes = _output.write(_set.begin()->data);
            _expect += write_bytes;
            _unassembled_bytes -= write_bytes;
            _set.erase(_set.begin());
        }
    }
    if (eof) {
        _eof_flag = true;
    }
    if (_eof_flag && empty()) {
        _output.end_input();
    }
}

size_t StreamReassembler::unassembled_bytes() const { return _unassembled_bytes; }

bool StreamReassembler::empty() const { return _unassembled_bytes == 0; }
