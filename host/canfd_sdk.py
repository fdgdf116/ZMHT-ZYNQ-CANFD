#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ZYNQ CANFD 上位机 SDK / 命令行工具

依赖：Python 3.7+，只用标准库（Windows / Linux 均可）。

协议要点（对应 CAN_FD协议文档.docx）：
  - 9009 下发命令，外层 cmd_code = 0x03CCF0FF
  - 9012 下发数据，外层 cmd_code = 0x04CCF0FF
  - 9010 上/下行，命令应答 0x05CCF0FF，数据上行 0x01CCF0FF
  - 外层 16 字节头，内层 112 字节包，全部大端
  - 9012 在外层头后额外带 4 字节通道号 uint32，不计入外层 length
  - 内层同步头 0x499602D2，校验为 Byte1~108 按 4 字节字异或

常用：
  python canfd_sdk.py selftest
  python canfd_sdk.py demo  --host 192.168.8.24 --channel 0 --bitrate 500000 --dbitrate 2000000 --id 0x123 --data 1122334455667788
  python canfd_sdk.py listen --host 192.168.8.24 --seconds 10
"""

import argparse
import queue
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, Iterator, List, Optional, Tuple

# ---------------------------------------------------------------------------
# 协议常量
# ---------------------------------------------------------------------------

CANFD_HEAD = 0x499602D2
CMD_CODE = 0x03CCF0FF
DATA_CODE = 0x04CCF0FF
RES_CODE = 0x05CCF0FF
UP_CODE = 0x01CCF0FF

CMD_PORT = 9009
UP_PORT = 9010
DATA_PORT = 9012

FRAME_SIZE = 112
NET_HEAD_SIZE = 16

CMD_CAN_BUF_QUERY = 0x0100
CMD_CAN_CONFIG = 0x0101
CMD_CAN_INFO_QUERY = 0x0102
CMD_CAN_START_STOP = 0x0103
CMD_VERSION_QUERY = 0xFF05

DATA_INDEX_NORMAL = 0x0001
DATA_INDEX_PULSE = 0x0002
DATA_INDEX_SIM = 0x0003

UPLINK_INDEX_STATUS = 0x0080
UPLINK_INDEX_RX = 0x0081
UPLINK_INDEX_SIM = 0x0082

TYPE_INIT = 0x00
TYPE_NORMAL = 0x01
TYPE_PULSE = 0x02
TYPE_SIM = 0x03
TYPE_STATUS = 0x80
TYPE_RX = 0x81
TYPE_SIM_UP = 0x82

MODE_NORMAL = 0x02
MODE_LOOPBACK = 0x04
RUN_OPEN = 0xAA
RUN_CLOSE = 0xBB

FD_LEN = (0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64)


class CanFdError(Exception):
    pass


class CanFdTimeout(CanFdError):
    pass


class ProtocolError(CanFdError):
    pass


# ---------------------------------------------------------------------------
# 基础编解码
# ---------------------------------------------------------------------------

def xor32(data: bytes) -> int:
    """协议校验：每 4 字节大端字异或。"""
    if len(data) % 4 != 0:
        raise ValueError("xor32 输入长度必须是 4 的倍数")
    value = 0
    for (word,) in struct.iter_unpack(">I", data):
        value ^= word
    return value


def pack_net_header(code: int, length: int, count: int = 0,
                    index: int = 0, timestamp: int = 0, crc: int = 0) -> bytes:
    return struct.pack(">IIHHHH", code, length, count & 0xFFFF,
                       index & 0xFFFF, timestamp & 0xFFFF, crc & 0xFFFF)


def pack_data_header(index: int, length: int, channel: int) -> bytes:
    header = bytearray(pack_net_header(DATA_CODE, length, 0, index))
    struct.pack_into(">I", header, 12, xor32(bytes(header[:12])))
    return bytes(header) + struct.pack(">I", channel)


def unpack_net_header(data: bytes) -> Tuple[int, int, int, int, int, int]:
    if len(data) != NET_HEAD_SIZE:
        raise ProtocolError("网络头长度不是 16 字节")
    return struct.unpack(">IIHHHH", data)


def dlc_to_len(dlc: int) -> int:
    return FD_LEN[dlc & 0x0F]


def len_to_dlc(length: int) -> int:
    if length <= 0:
        return 0
    for dlc, size in enumerate(FD_LEN):
        if length <= size:
            return dlc
    return 15


def idr_encode(can_id: int, extended: bool = False, remote: bool = False) -> int:
    if extended:
        return (((can_id >> 18) & 0x7FF) << 21) | (1 << 20) | (1 << 19) | \
               ((can_id & 0x3FFFF) << 1) | (1 if remote else 0)
    return ((can_id & 0x7FF) << 21) | ((1 << 20) if remote else 0)


def idr_decode(idr: int) -> Tuple[int, bool, bool]:
    extended = bool((idr >> 19) & 1)
    if extended:
        can_id = (((idr >> 21) & 0x7FF) << 18) | ((idr >> 1) & 0x3FFFF)
        remote = bool(idr & 1)
    else:
        can_id = (idr >> 21) & 0x7FF
        remote = bool((idr >> 20) & 1)
    return can_id, extended, remote


def dlcr_encode(fd: bool, brs: bool, dlc: int, esi: bool = False,
                timestamp: int = 0) -> int:
    value = (dlc & 0x0F) << 28
    if fd:
        value |= 1 << 27
    if brs:
        value |= 1 << 26
    if esi:
        value |= 1 << 25
    value |= timestamp & 0xFFFF
    return value


def dlcr_decode(dlcr: int) -> Tuple[int, bool, bool, bool, int]:
    dlc = (dlcr >> 28) & 0x0F
    fd = bool((dlcr >> 27) & 1)
    brs = bool((dlcr >> 26) & 1)
    esi = bool((dlcr >> 25) & 1)
    timestamp = dlcr & 0xFFFF
    return dlc, fd, brs, esi, timestamp


def _finish_inner(packet: bytearray) -> bytes:
    if len(packet) != FRAME_SIZE:
        raise ValueError("内层包必须是 112 字节")
    struct.pack_into(">I", packet, 108, xor32(bytes(packet[:108])))
    return bytes(packet)


# ---------------------------------------------------------------------------
# 数据模型
# ---------------------------------------------------------------------------

@dataclass
class CanFdFrame:
    channel: int
    can_id: int
    data: bytes = b""
    fd: bool = True
    brs: bool = False
    extended: bool = False
    remote: bool = False
    esi: bool = False
    dlc: int = 0
    timestamp_us: int = 0
    total_count: int = 0
    ch_count: int = 0
    states: int = 0
    xor_states: int = 0
    packets_sum: int = 1
    packets_no: int = 0
    msg_type: int = TYPE_RX

    @property
    def length(self) -> int:
        return len(self.data)

    @classmethod
    def from_inner(cls, packet: bytes, outer_index: Optional[int] = None) -> "CanFdFrame":
        if len(packet) != FRAME_SIZE:
            raise ProtocolError("内层帧长度不是 112 字节")
        head = struct.unpack_from(">I", packet, 0)[0]
        if head != CANFD_HEAD:
            raise ProtocolError("内层同步头错误: 0x%08X" % head)
        msg_type = packet[5]
        total, ch_count = struct.unpack_from(">II", packet, 8)
        idr, dlcr = struct.unpack_from(">II", packet, 16)
        can_id, extended, remote = idr_decode(idr)
        dlc, fd, brs, esi, hw_ts = dlcr_decode(dlcr)
        length = dlc_to_len(dlc) if fd else min(dlc, 8)
        data = bytes(packet[24:24 + length])
        timestamp_us = struct.unpack_from(">Q", packet, 100)[0]
        states = packet[96] if msg_type in (TYPE_STATUS, TYPE_RX) else 0
        xor_states = packet[97] if msg_type == TYPE_STATUS else 0
        packets_sum = struct.unpack_from(">H", packet, 96)[0] \
            if msg_type in (TYPE_NORMAL, TYPE_PULSE) else 1
        packets_no = struct.unpack_from(">H", packet, 98)[0] \
            if msg_type in (TYPE_NORMAL, TYPE_PULSE) else 0
        return cls(
            channel=packet[7], can_id=can_id, data=data, fd=fd, brs=brs,
            extended=extended, remote=remote, esi=esi, dlc=dlc,
            timestamp_us=timestamp_us, total_count=total, ch_count=ch_count,
            states=states, xor_states=xor_states, packets_sum=packets_sum,
            packets_no=packets_no, msg_type=msg_type,
        )

    def __str__(self) -> str:
        kind = "FD" if self.fd else "CAN"
        flags = []
        if self.extended:
            flags.append("EXT")
        if self.remote:
            flags.append("RTR")
        if self.brs:
            flags.append("BRS")
        if self.esi:
            flags.append("ESI")
        if self.msg_type == TYPE_STATUS:
            flags.append("TX_OK" if self.states else "TX_FAIL")
        return ("ch%d %s id=0x%X%s len=%d data=%s ts=%d cnt=%d/%d%s" % (
            self.channel, kind, self.can_id,
            (" " + ",".join(flags)) if flags else "",
            len(self.data), self.data.hex().upper(), self.timestamp_us,
            self.total_count, self.ch_count,
            (" states=%d" % self.states) if self.msg_type in (TYPE_STATUS, TYPE_RX) else "",
        ))


@dataclass
class ChannelConfig:
    channel: int
    configured: bool = False
    bitrate: int = 0
    data_bitrate: int = 0
    mode: int = 0
    auto_send: int = 0
    ier: int = 0
    packet_interval_us: int = 0
    brs_en: int = 0
    timestamp_us: int = 0

    @classmethod
    def from_inner(cls, packet: bytes, channel: int) -> "ChannelConfig":
        if len(packet) != FRAME_SIZE:
            raise ProtocolError("配置记录长度不是 112 字节")
        if struct.unpack_from(">I", packet, 0)[0] != CANFD_HEAD:
            return cls(channel=channel, configured=False)
        return cls(
            channel=packet[7] if packet[4] == 1 else channel,
            configured=True,
            bitrate=struct.unpack_from(">I", packet, 16)[0],
            mode=packet[20], auto_send=packet[21],
            ier=struct.unpack_from(">H", packet, 22)[0],
            packet_interval_us=struct.unpack_from(">I", packet, 24)[0],
            data_bitrate=struct.unpack_from(">I", packet, 28)[0],
            brs_en=packet[32],
            timestamp_us=struct.unpack_from(">Q", packet, 100)[0],
        )


@dataclass
class BufferStatus:
    channel: int
    normal_space: int
    pulse_space: int
    sim_space: int

    @classmethod
    def from_bytes(cls, data: bytes) -> "BufferStatus":
        if len(data) != 16:
            raise ProtocolError("缓冲查询记录长度不是 16 字节")
        return cls(channel=data[0],
                   normal_space=struct.unpack_from(">I", data, 4)[0],
                   pulse_space=struct.unpack_from(">I", data, 8)[0],
                   sim_space=struct.unpack_from(">I", data, 12)[0])


# ---------------------------------------------------------------------------
# 组包
# ---------------------------------------------------------------------------

def build_config_packet(channel: int, bitrate: int, data_bitrate: int = 0,
                        mode: int = MODE_NORMAL, auto_send: bool = True,
                        brs: bool = False, ier: int = 0,
                        packet_interval_us: int = 0,
                        timestamp_us: int = 0) -> bytes:
    packet = bytearray(FRAME_SIZE)
    struct.pack_into(">I", packet, 0, CANFD_HEAD)
    packet[4] = 0                 # 上位机 -> 下位机
    packet[5] = TYPE_INIT
    packet[6] = FRAME_SIZE
    packet[7] = channel & 0xFF
    struct.pack_into(">I", packet, 16, bitrate)
    packet[20] = mode & 0xFF
    packet[21] = 1 if auto_send else 0
    struct.pack_into(">H", packet, 22, ier & 0x0FFF)
    struct.pack_into(">I", packet, 24, packet_interval_us & 0xFFFFFFFF)
    struct.pack_into(">I", packet, 28, data_bitrate & 0xFFFFFFFF)
    packet[32] = 1 if brs else 0
    struct.pack_into(">Q", packet, 100, timestamp_us & 0xFFFFFFFFFFFFFFFF)
    return _finish_inner(packet)


def build_data_packet(channel: int, can_id: int, data: bytes = b"",
                      fd: bool = True, brs: bool = False,
                      extended: bool = False, remote: bool = False,
                      total_count: int = 0, ch_count: int = 0,
                      packets_sum: int = 1, packets_no: int = 0,
                      msg_type: int = TYPE_NORMAL,
                      timestamp_us: int = 0) -> bytes:
    if len(data) > 64:
        raise ValueError("CAN FD 数据最多 64 字节")
    if not fd and len(data) > 8:
        raise ValueError("经典 CAN 数据最多 8 字节")
    if remote:
        data = b""
    if fd:
        dlc = len_to_dlc(len(data))
    else:
        dlc = min(len(data), 8)
    packet = bytearray(FRAME_SIZE)
    struct.pack_into(">I", packet, 0, CANFD_HEAD)
    packet[4] = 0                 # 上位机 -> 下位机
    packet[5] = msg_type & 0xFF
    packet[6] = FRAME_SIZE
    packet[7] = channel & 0xFF
    struct.pack_into(">II", packet, 8, total_count & 0xFFFFFFFF, ch_count & 0xFFFFFFFF)
    struct.pack_into(">I", packet, 16, idr_encode(can_id, extended, remote))
    struct.pack_into(">I", packet, 20, dlcr_encode(fd, brs, dlc))
    packet[24:24 + len(data)] = data
    struct.pack_into(">HH", packet, 96, packets_sum & 0xFFFF, packets_no & 0xFFFF)
    struct.pack_into(">Q", packet, 100, timestamp_us & 0xFFFFFFFFFFFFFFFF)
    return _finish_inner(packet)


# ---------------------------------------------------------------------------
# 客户端
# ---------------------------------------------------------------------------

class CanFdClient:
    """
    ZYNQ CANFD 控制盒上位机客户端。

    典型流程（必须先连 9010，再发命令）：
        with CanFdClient("192.168.8.24") as c:
            c.configure(0, 500000, 2000000)
            c.start(0)
            fb = c.send(0, 0x123, b"\\x01\\x02")
            frame = c.receive(timeout=2.0)
    """

    def __init__(self, host: str, cmd_port: int = CMD_PORT,
                 up_port: int = UP_PORT, data_port: int = DATA_PORT,
                 timeout: float = 3.0, ready_delay: float = 0.3):
        self.host = host
        self.cmd_port = cmd_port
        self.up_port = up_port
        self.data_port = data_port
        self.timeout = timeout
        self.ready_delay = ready_delay

        self._cmd_sock: Optional[socket.socket] = None
        self._up_sock: Optional[socket.socket] = None
        self._data_sock: Optional[socket.socket] = None
        self._reader: Optional[threading.Thread] = None
        self._stop = threading.Event()

        self._cmd_lock = threading.Lock()
        self._pending: Dict[Tuple[int, int], Tuple[threading.Event, dict]] = {}
        self._pending_lock = threading.Lock()
        self._tx_pending: Dict[Tuple[int, int], Tuple[threading.Event, dict]] = {}
        self._tx_lock = threading.Lock()

        self._rxq: "queue.Queue[CanFdFrame]" = queue.Queue()
        self._feedbackq: "queue.Queue[CanFdFrame]" = queue.Queue()
        self._cmd_count = 0
        self._total_tx = 0
        self._ch_tx = [0] * 6
        self._on_frame: Optional[Callable[[CanFdFrame], None]] = None

    # -- 连接 ------------------------------------------------------------
    def open(self) -> "CanFdClient":
        if self._up_sock is not None:
            return self
        # 9010 必须先连：下位机在 9010 没有客户端时会丢弃所有应答/上行。
        self._up_sock = socket.create_connection((self.host, self.up_port), self.timeout)
        self._up_sock.settimeout(None)
        self._up_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        time.sleep(self.ready_delay)

        self._cmd_sock = socket.create_connection((self.host, self.cmd_port), self.timeout)
        self._cmd_sock.settimeout(None)
        self._cmd_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        self._data_sock = socket.create_connection((self.host, self.data_port), self.timeout)
        self._data_sock.settimeout(None)
        self._data_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        self._stop.clear()
        self._reader = threading.Thread(target=self._reader_loop, daemon=True,
                                        name="canfd-uplink")
        self._reader.start()
        return self

    def close(self) -> None:
        self._stop.set()
        for sock in (self._cmd_sock, self._data_sock, self._up_sock):
            if sock is not None:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    sock.close()
                except OSError:
                    pass
        self._cmd_sock = self._data_sock = self._up_sock = None
        if self._reader is not None:
            self._reader.join(timeout=2.0)
            self._reader = None
        with self._pending_lock:
            self._pending.clear()
        with self._tx_lock:
            self._tx_pending.clear()

    def __enter__(self) -> "CanFdClient":
        return self.open()

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    # -- 内部收发 --------------------------------------------------------
    @staticmethod
    def _recv_exact(sock: socket.socket, size: int) -> bytes:
        chunks = []
        remaining = size
        while remaining:
            chunk = sock.recv(remaining)
            if not chunk:
                raise ConnectionError("连接被下位机关闭")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _reader_loop(self) -> None:
        sock = self._up_sock
        while not self._stop.is_set():
            try:
                header = self._recv_exact(sock, NET_HEAD_SIZE)
                code, length, count, index, _timestamp, _crc = unpack_net_header(header)
                if code == RES_CODE:
                    payload = self._recv_exact(sock, length) if length else b""
                    with self._pending_lock:
                        entry = self._pending.get((index, count))
                    if entry is not None:
                        event, box = entry
                        box["payload"] = payload
                        event.set()
                    continue
                if code == UP_CODE:
                    if length == 0:
                        continue
                    wire_channel = None
                    if index in (UPLINK_INDEX_STATUS, UPLINK_INDEX_RX):
                        wire_channel = struct.unpack(
                            ">I", self._recv_exact(sock, 4))[0]
                    payload = self._recv_exact(sock, length)
                    if index not in (UPLINK_INDEX_STATUS, UPLINK_INDEX_RX,
                                     UPLINK_INDEX_SIM):
                        continue
                    try:
                        frame = CanFdFrame.from_inner(payload, index)
                    except ProtocolError:
                        continue
                    if wire_channel is not None and frame.channel != wire_channel:
                        continue
                    if index == UPLINK_INDEX_STATUS:
                        with self._tx_lock:
                            entry = self._tx_pending.get((frame.channel, frame.ch_count))
                        if entry is not None:
                            event, box = entry
                            box["frame"] = frame
                            event.set()
                        else:
                            self._feedbackq.put(frame)
                    else:
                        self._rxq.put(frame)
                        cb = self._on_frame
                        if cb is not None:
                            try:
                                cb(frame)
                            except Exception:
                                pass
                    continue
                # 未知 cmd_code：读掉载荷，避免流错位
                if length:
                    self._recv_exact(sock, length)
            except (OSError, ConnectionError):
                if not self._stop.is_set():
                    self._stop.set()
                break

    def _request(self, index: int, payload: bytes, timeout: Optional[float] = None) -> bytes:
        if self._cmd_sock is None:
            raise CanFdError("未连接，请先 open()")
        timeout = self.timeout if timeout is None else timeout
        with self._cmd_lock:
            self._cmd_count = (self._cmd_count + 1) & 0xFFFF
            count = self._cmd_count
            event = threading.Event()
            box = {}
            key = (index, count)
            with self._pending_lock:
                self._pending[key] = (event, box)
            try:
                self._cmd_sock.sendall(
                    pack_net_header(CMD_CODE, len(payload), count, index) + payload)
                if not event.wait(timeout):
                    raise CanFdTimeout(
                        "命令 0x%04X 超时：先确认 9010 已连上且下位机在运行" % index)
                return bytes(box.get("payload", b""))
            finally:
                with self._pending_lock:
                    self._pending.pop(key, None)

    @staticmethod
    def _status(payload: bytes) -> int:
        if len(payload) < 4:
            raise ProtocolError("应答不足 4 字节")
        return struct.unpack_from(">I", payload, 0)[0]

    # -- 命令 ------------------------------------------------------------
    def configure(self, channel: int, bitrate: int, data_bitrate: int = 0,
                  mode: int = MODE_NORMAL, auto_send: bool = True,
                  brs: bool = False, ier: int = 0,
                  packet_interval_us: int = 0) -> int:
        packet = build_config_packet(channel, bitrate, data_bitrate, mode,
                                     auto_send, brs, ier, packet_interval_us)
        return self._status(self._request(CMD_CAN_CONFIG, packet))

    def start(self, channel: int) -> int:
        return self._status(self._request(
            CMD_CAN_START_STOP, struct.pack(">II", channel, RUN_OPEN)))

    def stop(self, channel: int) -> int:
        return self._status(self._request(
            CMD_CAN_START_STOP, struct.pack(">II", channel, RUN_CLOSE)))

    def query_info(self) -> List[ChannelConfig]:
        payload = self._request(CMD_CAN_INFO_QUERY, b"\xff\xff\xff\xff")
        if len(payload) == 4:
            raise CanFdError("信息查询失败，状态=%d" % self._status(payload))
        if len(payload) != 6 * FRAME_SIZE:
            raise ProtocolError("信息查询应答长度=%d，预期 672" % len(payload))
        return [ChannelConfig.from_inner(payload[i * FRAME_SIZE:(i + 1) * FRAME_SIZE], i)
                for i in range(6)]

    def query_buffers(self) -> List[BufferStatus]:
        payload = self._request(CMD_CAN_BUF_QUERY, b"\xff\xff\xff\xff")
        if len(payload) == 4:
            raise CanFdError("缓冲查询失败，状态=%d" % self._status(payload))
        if len(payload) != 6 * 16:
            raise ProtocolError("缓冲查询应答长度=%d，预期 96" % len(payload))
        return [BufferStatus.from_bytes(payload[i * 16:(i + 1) * 16])
                for i in range(6)]

    def version(self, target: str = "arm") -> int:
        value = 0xAA if target.lower() == "arm" else 0xBB
        payload = self._request(CMD_VERSION_QUERY, struct.pack(">I", value))
        return self._status(payload)

    # -- 发送 ------------------------------------------------------------
    def send(self, channel: int, can_id: int, data: bytes = b"",
             fd: bool = True, brs: bool = False, extended: bool = False,
             remote: bool = False, pulse: bool = False,
             packet_interval_us: Optional[int] = None,
             timeout: Optional[float] = None) -> CanFdFrame:
        if isinstance(data, str):
            data = bytes.fromhex(data.replace(" ", "").replace(":", ""))
        if self._data_sock is None:
            raise CanFdError("未连接，请先 open()")
        if packet_interval_us:
            time.sleep(packet_interval_us / 1_000_000.0)
        self._total_tx = (self._total_tx + 1) & 0xFFFFFFFF
        self._ch_tx[channel] = (self._ch_tx[channel] + 1) & 0xFFFFFFFF
        total_count = self._total_tx
        ch_count = self._ch_tx[channel]
        msg_type = TYPE_PULSE if pulse else TYPE_NORMAL
        index = DATA_INDEX_PULSE if pulse else DATA_INDEX_NORMAL
        packet = build_data_packet(
            channel, can_id, data, fd=fd, brs=brs, extended=extended,
            remote=remote, total_count=total_count, ch_count=ch_count,
            msg_type=msg_type)

        event = threading.Event()
        box = {}
        key = (channel, ch_count)
        with self._tx_lock:
            self._tx_pending[key] = (event, box)
        try:
            self._data_sock.sendall(
                pack_data_header(index, len(packet), channel) + packet)
            if not event.wait(self.timeout if timeout is None else timeout):
                raise CanFdTimeout(
                    "未收到 0x%02X 通道 %d 的发送反馈：确认通道已 start、"
                    "帧校验正确且 canX 处于运行状态" % (UPLINK_INDEX_STATUS, channel))
            return box["frame"]
        finally:
            with self._tx_lock:
                self._tx_pending.pop(key, None)

    # -- 接收 ------------------------------------------------------------
    def receive(self, timeout: Optional[float] = None) -> Optional[CanFdFrame]:
        """等待一帧 0x81 采集上行；超时返回 None。"""
        try:
            return self._rxq.get(timeout=self.timeout if timeout is None else timeout)
        except queue.Empty:
            return None

    def receive_feedback(self, timeout: Optional[float] = None) -> Optional[CanFdFrame]:
        try:
            return self._feedbackq.get(timeout=self.timeout if timeout is None else timeout)
        except queue.Empty:
            return None

    def frames(self) -> Iterator[CanFdFrame]:
        while not self._stop.is_set():
            frame = self.receive(timeout=0.5)
            if frame is not None:
                yield frame

    def set_frame_callback(self, callback: Optional[Callable[[CanFdFrame], None]]) -> None:
        self._on_frame = callback


# ---------------------------------------------------------------------------
# 命令行
# ---------------------------------------------------------------------------

def _parse_data(text: str) -> bytes:
    cleaned = text.replace(" ", "").replace(":", "").replace(",", "")
    if len(cleaned) % 2:
        raise argparse.ArgumentTypeError("数据必须是偶数个十六进制字符")
    try:
        return bytes.fromhex(cleaned)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("数据不是合法十六进制: %s" % exc)


def _client(args) -> CanFdClient:
    return CanFdClient(args.host, timeout=args.timeout)


def cmd_selftest(_args) -> int:
    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    # 网络头
    raw = pack_net_header(CMD_CODE, 112, 0x1234, CMD_CAN_CONFIG, 0xABCD)
    check("header bytes", raw == bytes.fromhex("03CCF0FF0000007012340101ABCD0000"))
    check("header roundtrip", unpack_net_header(raw)[:5] ==
          (CMD_CODE, 112, 0x1234, CMD_CAN_CONFIG, 0xABCD))
    data_header = pack_data_header(DATA_INDEX_NORMAL, 112, 0)
    check("data header", data_header == bytes.fromhex(
        "04CCF0FF000000700000000104CCF08E00000000"))

    # IDR
    check("idr std", idr_encode(0x123) == (0x123 << 21))
    check("idr std rtr", idr_encode(0x123, remote=True) == ((0x123 << 21) | (1 << 20)))
    ext = 0x1ABCDEF
    check("idr ext roundtrip", idr_decode(idr_encode(ext, True)) == (ext, True, False))
    check("idr ext sr/ide", (idr_encode(ext, True) >> 19) & 0x3 == 0x3)

    # DLCR / DLC
    check("dlc table", [dlc_to_len(i) for i in (8, 9, 10, 15)] == [8, 12, 16, 64])
    check("len to dlc", [len_to_dlc(v) for v in (0, 8, 9, 21, 64)] == [0, 8, 9, 12, 15])
    check("dlcr fd64", dlcr_encode(True, True, 15) == 0xFC000000)
    check("dlcr decode", dlcr_decode(0xFC000000) == (15, True, True, False, 0))

    # 配置包
    cfg = build_config_packet(2, 500000, 2000000, brs=True)
    check("config size", len(cfg) == 112)
    check("config head", cfg[:4] == bytes.fromhex("499602D2"))
    check("config offsets", (cfg[4], cfg[5], cfg[6], cfg[7]) == (0, 0, 112, 2))
    check("config bitrate", struct.unpack_from(">I", cfg, 16)[0] == 500000)
    check("config dbitrate", struct.unpack_from(">I", cfg, 28)[0] == 2000000)
    check("config checksum", struct.unpack_from(">I", cfg, 108)[0] == xor32(cfg[:108]))

    # 数据包往返
    payload = bytes(range(64))
    pkt = build_data_packet(3, ext, payload, fd=True, brs=True, extended=True,
                            total_count=7, ch_count=9)
    frame = CanFdFrame.from_inner(pkt, UPLINK_INDEX_RX)
    check("data roundtrip", (frame.channel, frame.can_id, frame.extended,
                             frame.fd, frame.brs, frame.data) ==
          (3, ext, True, True, True, payload))
    check("data counters", (frame.total_count, frame.ch_count) == (7, 9))
    check("data checksum", struct.unpack_from(">I", pkt, 108)[0] == xor32(pkt[:108]))

    # 经典帧
    pkt = build_data_packet(0, 0x321, b"\x01\x02\x03", fd=False)
    frame = CanFdFrame.from_inner(pkt)
    check("classic", (frame.fd, frame.can_id, frame.data) == (False, 0x321, b"\x01\x02\x03"))

    if failures:
        print("SELFTEST FAILED:", ", ".join(failures))
        return 1
    print("selftest passed")
    return 0


def cmd_demo(args) -> int:
    data = _parse_data(args.data) if args.data else bytes(range(8))
    with _client(args) as client:
        print("[1] configure ch%d bitrate=%d dbitrate=%d mode=0x%02X" %
              (args.channel, args.bitrate, args.dbitrate, args.mode))
        status = client.configure(args.channel, args.bitrate, args.dbitrate,
                                  mode=args.mode, auto_send=not args.no_auto_send,
                                  brs=args.brs_en)
        print("    status = %d" % status)
        if status != 0:
            return 1

        print("[2] start ch%d" % args.channel)
        status = client.start(args.channel)
        print("    status = %d" % status)
        if status != 0:
            return 1

        try:
            print("[3] send id=0x%X data=%s fd=%s brs=%s" %
                  (args.can_id, data.hex().upper(), not args.classic, args.brs))
            feedback = client.send(args.channel, args.can_id, data,
                                   fd=not args.classic, brs=args.brs,
                                   extended=args.extended,
                                   timeout=args.feedback_timeout)
            print("    feedback:", feedback)
            print("    States = %d (%s)" %
                  (feedback.states, "发送成功" if feedback.states else "发送失败"))

            print("[4] listen %d s ..." % args.listen)
            end = time.monotonic() + args.listen
            while time.monotonic() < end:
                frame = client.receive(timeout=min(0.5, end - time.monotonic()))
                if frame is not None:
                    print("    RX:", frame)
        finally:
            print("[5] stop ch%d" % args.channel)
            try:
                print("    status = %d" % client.stop(args.channel))
            except CanFdError as exc:
                print("    stop failed:", exc)
    return 0


def cmd_send(args) -> int:
    data = _parse_data(args.data) if args.data else b""
    with _client(args) as client:
        if args.init:
            status = client.configure(args.channel, args.bitrate, args.dbitrate,
                                      mode=args.mode)
            print("configure status =", status)
            if status != 0:
                return 1
            status = client.start(args.channel)
            print("start status =", status)
            if status != 0:
                return 1
        feedback = client.send(args.channel, args.can_id, data,
                               fd=not args.classic, brs=args.brs,
                               extended=args.extended,
                               timeout=args.feedback_timeout)
        print(feedback)
        print("States =", feedback.states)
    return 0


def cmd_listen(args) -> int:
    with _client(args) as client:
        print("listening on %s:9010 for %d s ..." % (args.host, args.seconds))
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            frame = client.receive(timeout=min(0.5, end - time.monotonic()))
            if frame is not None:
                print(frame)
    return 0


def cmd_info(args) -> int:
    with _client(args) as client:
        print("version(ARM) = 0x%08X" % client.version("arm"))
        for cfg in client.query_info():
            if cfg.configured:
                print("ch%d: bitrate=%d dbitrate=%d mode=0x%02X brs=%d" %
                      (cfg.channel, cfg.bitrate, cfg.data_bitrate, cfg.mode, cfg.brs_en))
            else:
                print("ch%d: <未配置>" % cfg.channel)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ZYNQ CANFD 上位机 SDK/工具")
    sub = parser.add_subparsers(dest="command", required=True)

    def add_common(p):
        p.add_argument("--host", default="192.168.8.24", help="板子 IP")
        p.add_argument("--timeout", type=float, default=3.0, help="命令超时秒数")

    p = sub.add_parser("selftest")
    p.set_defaults(func=cmd_selftest)

    p = sub.add_parser("demo", help="配置+开通道+发一帧+监听")
    add_common(p)
    p.add_argument("--channel", type=int, default=0)
    p.add_argument("--bitrate", type=int, default=500000)
    p.add_argument("--dbitrate", type=int, default=2000000)
    p.add_argument("--mode", type=lambda v: int(v, 0), default=MODE_NORMAL)
    p.add_argument("--id", dest="can_id", type=lambda v: int(v, 0), default=0x123)
    p.add_argument("--data", default="", help="十六进制数据，如 1122334455")
    p.add_argument("--classic", action="store_true", help="发经典帧")
    p.add_argument("--brs", action="store_true", help="帧带 BRS 标志")
    p.add_argument("--brs-en", action="store_true", help="初始化时使能 BRS")
    p.add_argument("--extended", action="store_true", help="29 位扩展帧")
    p.add_argument("--no-auto-send", action="store_true", help="关闭自动重传")
    p.add_argument("--listen", type=int, default=10, help="发送后监听秒数")
    p.add_argument("--feedback-timeout", type=float, default=3.0)
    p.set_defaults(func=cmd_demo)

    p = sub.add_parser("send", help="发一帧（假定已配置并 start）")
    add_common(p)
    p.add_argument("--channel", type=int, default=0)
    p.add_argument("--id", dest="can_id", type=lambda v: int(v, 0), required=True)
    p.add_argument("--data", default="")
    p.add_argument("--classic", action="store_true")
    p.add_argument("--brs", action="store_true")
    p.add_argument("--extended", action="store_true")
    p.add_argument("--init", action="store_true", help="先 0x0101 配置并 0x0103 打开")
    p.add_argument("--bitrate", type=int, default=500000)
    p.add_argument("--dbitrate", type=int, default=2000000)
    p.add_argument("--mode", type=lambda v: int(v, 0), default=MODE_NORMAL)
    p.add_argument("--feedback-timeout", type=float, default=3.0)
    p.set_defaults(func=cmd_send)

    p = sub.add_parser("listen", help="只监听 9010 的 0x81 上行")
    add_common(p)
    p.add_argument("--seconds", type=int, default=10)
    p.set_defaults(func=cmd_listen)

    p = sub.add_parser("info", help="查询版本与通道配置")
    add_common(p)
    p.set_defaults(func=cmd_info)
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except CanFdError as exc:
        print("ERROR:", exc, file=sys.stderr)
        return 2
    except (OSError, ConnectionError) as exc:
        print("NETWORK ERROR:", exc, file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
