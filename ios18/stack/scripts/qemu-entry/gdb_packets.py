"""Buffered packet reception for the bounded QEMU GDB observer."""


class Packets:
    def __init__(self, connection):
        self.connection = connection
        self.pending = bytearray()

    def request(self, value):
        if value is None:
            self.connection.settimeout(3)
            self.connection.sendall(b'\x03')
        else:
            payload = value.encode()
            self.connection.sendall(b'$' + payload + b'#' + f'{sum(payload) % 256:02x}'.encode())
        return self.receive()

    def receive(self):
        # Retain partial packets across socket timeouts. The caller may interrupt
        # a running target afterwards; socket.makefile would become unusable.
        while True:
            start = self.pending.find(b'$')
            if start < 0:
                self.pending.clear()  # acknowledgements, before a reply
            else:
                del self.pending[:start]
                end = self.pending.find(b'#', 1)
                if end > 1024 * 1024:
                    raise RuntimeError('GDB response exceeds observer bound')
                if end >= 0 and len(self.pending) >= end + 3:
                    data = self.pending[1:end]
                    checksum = self.pending[end + 1:end + 3]
                    del self.pending[:end + 3]
                    if int(checksum, 16) != sum(data) % 256:
                        raise RuntimeError('GDB checksum mismatch')
                    self.connection.sendall(b'+')
                    return data.decode()
                if len(self.pending) > 1024 * 1024:
                    raise RuntimeError('GDB response exceeds observer bound')
            chunk = self.connection.recv(65536)
            if not chunk:
                raise RuntimeError('GDB connection closed')
            self.pending.extend(chunk)
