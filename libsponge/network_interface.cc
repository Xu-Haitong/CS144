#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>

// Dummy implementation of a network interface
// Translates from {IP datagram, next hop address} to link-layer frame, and from link-layer frame to IP datagram

// For Lab 5, please replace with a real implementation that passes the
// automated checks run by `make check_lab5`.

// You will need to add private members to the class declaration in `network_interface.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    // convert IP address of next hop to raw 32-bit representation (used in ARP header)
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();
    EthernetFrame frame;
    frame.payload() = dgram.serialize();
    frame.header().type = EthernetHeader::TYPE_IPv4;
    frame.header().src = _ethernet_address;
    if (_arp_table.count(next_hop_ip)) {
        auto item = _arp_table[next_hop_ip];
        frame.header().dst = item.address;
        _frames_out.push(frame);
    } else {
        _waiting_frames[next_hop_ip].push_back(frame);
        // 当前没有 next_hop_ip 的记录，需要 ARP 解析
        // 对同一 IP 的 ARP 解析请求至少相隔 5000 ms
        if (_last_arp.count(next_hop_ip) == 0 || _last_arp[next_hop_ip] + 5000 <= _ticks) {
            ARPMessage arp_req;
            arp_req.sender_ip_address = _ip_address.ipv4_numeric();
            arp_req.target_ip_address = next_hop_ip;
            arp_req.sender_ethernet_address = _ethernet_address;
            arp_req.opcode = ARPMessage::OPCODE_REQUEST;
            EthernetFrame arp_frame;
            arp_frame.payload() = arp_req.serialize();
            arp_frame.header().type = EthernetHeader::TYPE_ARP;
            arp_frame.header().src = _ethernet_address;
            arp_frame.header().dst = ETHERNET_BROADCAST;
            _frames_out.push(arp_frame);

            _last_arp[next_hop_ip] = _ticks;
        }
    }
}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    if (frame.header().dst != _ethernet_address && frame.header().dst != ETHERNET_BROADCAST) {
        return nullopt;
    }
    if (frame.header().type == EthernetHeader::TYPE_IPv4) {
        InternetDatagram dgram;
        if (dgram.parse(frame.payload()) == ParseResult::NoError) {
            return dgram;
        }
        return nullopt;
    }
    if (frame.header().type == EthernetHeader::TYPE_ARP) {
        ARPMessage arp;
        if (arp.parse(frame.payload()) == ParseResult::NoError) {
            // 如果来的 ARPMessage 是没有见过的，需要更新 _waiting_frames
            if (_arp_table.count(arp.sender_ip_address) == 0) {
                for (auto waiting_frame : _waiting_frames[arp.sender_ip_address]) {
                    waiting_frame.header().dst = arp.sender_ethernet_address;
                    _frames_out.push(waiting_frame);
                }
                _waiting_frames.erase(arp.sender_ip_address);
            }
            _arp_table[arp.sender_ip_address] = {arp.sender_ethernet_address, _ticks + 30 * 1000};
            // 如果来的 ARPMessage 是对自己的 request
            if (arp.target_ip_address == _ip_address.ipv4_numeric() && arp.opcode == ARPMessage::OPCODE_REQUEST) {
                ARPMessage arp_reply;
                arp_reply.sender_ip_address = _ip_address.ipv4_numeric();
                arp_reply.target_ip_address = arp.sender_ip_address;
                arp_reply.sender_ethernet_address = _ethernet_address;
                arp_reply.target_ethernet_address = arp.sender_ethernet_address;
                arp_reply.opcode = ARPMessage::OPCODE_REPLY;
                EthernetFrame reply_frame;
                reply_frame.payload() = arp_reply.serialize();
                reply_frame.header().type = EthernetHeader::TYPE_ARP;
                reply_frame.header().src = _ethernet_address;
                reply_frame.header().dst = arp.sender_ethernet_address;
                _frames_out.push(reply_frame);
            }
        }
    }
    return nullopt;
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    _ticks += ms_since_last_tick;
    for (auto item : _arp_table) {
        if (_ticks >= item.second._expire_time) {
            _arp_table.erase(item.first);
        }
    }
}