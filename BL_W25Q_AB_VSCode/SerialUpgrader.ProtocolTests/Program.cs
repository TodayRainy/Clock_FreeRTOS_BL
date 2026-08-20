using SerialUpgrader;

static void RequireEqual(string name, byte[] actual, byte[] expected)
{
    if (!actual.AsSpan().SequenceEqual(expected))
    {
        throw new InvalidOperationException(
            $"{name}: expected {Convert.ToHexString(expected)}, " +
            $"got {Convert.ToHexString(actual)}");
    }
}

/*
 * 请求帧没有响应帧中的 ErrorCode 字节。这几个固定向量锁定两种帧的差异，
 * 防止维护响应解析时再次误改请求 Length 和 Payload 偏移。
 */
RequireEqual(
    "GET_VERSION",
    Transfer.BuildRequestPacket(0x01, [0x00]),
    [0xAA, 0x01, 0x01, 0x00, 0x00, 0x4E, 0x2D]);
RequireEqual(
    "SET_BAUD 2000000",
    Transfer.BuildRequestPacket(0x02, [0x80, 0x84, 0x1E, 0x00]),
    [0xAA, 0x02, 0x04, 0x00, 0x80, 0x84, 0x1E, 0x00, 0x09, 0x6B]);
RequireEqual(
    "BOOT",
    Transfer.BuildRequestPacket(0x22, null),
    [0xAA, 0x22, 0x00, 0x00, 0x7B, 0x6A]);

if (CRC.CRC16("123456789"u8) != 0x31C3u ||
    CRC.CRC32("123456789"u8) != 0xCBF43926u)
{
    throw new InvalidOperationException("CRC known-answer test failed");
}

Console.WriteLine("Serial protocol frame tests passed.");
