"""Transport regressions: fragmented reads, batched replies and timeout recovery."""
import socket
import unittest

from gdb_packets import Packets


def frame(data):
    return b'$' + data + b'#' + f'{sum(data) % 256:02x}'.encode()


class Socket:
    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.sent = []
        self.reads = 0

    def recv(self, _):
        self.reads += 1
        chunk = next(self.chunks, b'')
        if isinstance(chunk, Exception):
            raise chunk
        return chunk

    def sendall(self, data):
        self.sent.append(data)

    def settimeout(self, value):
        self.timeout = value


class Tests(unittest.TestCase):
    def test_fragmented_checksum_and_batched_reply(self):
        data = b'+' + frame(b'12345678') + frame(b'OK')
        connection = Socket([data[:4], data[4:13], data[13:]])
        packets = Packets(connection)
        self.assertEqual(packets.request('m1000,4'), '12345678')
        self.assertEqual(packets.request('qAttached'), 'OK')
        self.assertEqual(connection.sent.count(b'+'), 2)

    def test_timeout_keeps_partial_reply(self):
        data = frame(b'T05thread:1;')
        connection = Socket([data[:6], socket.timeout(), data[6:]])
        packets = Packets(connection)
        with self.assertRaises(socket.timeout):
            packets.request('c')
        self.assertEqual(packets.request(None), 'T05thread:1;')
        self.assertEqual(connection.sent[-2:], [b'\x03', b'+'])

    def test_large_memory_reply_is_read_in_chunks(self):
        data = b'ab' * 16384
        connection = Socket([frame(data)])
        self.assertEqual(Packets(connection).request('m1000,4000'), data.decode())
        self.assertEqual(connection.reads, 1)

    def test_corruption_and_truncation_are_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'checksum'):
            Packets(Socket([b'$1234#00'])).request('m1000,2')
        with self.assertRaisesRegex(RuntimeError, 'closed'):
            Packets(Socket([b'$1234#'])).request('m1000,2')


if __name__ == '__main__':
    unittest.main()
