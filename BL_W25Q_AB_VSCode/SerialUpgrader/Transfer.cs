using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using System.Threading.Channels;

namespace SerialUpgrader;

/// <summary>Bootloader 持久化的 A/B 启动阶段。</summary>
public enum BootPhase : byte
{
    Normal = 0,
    InstallPending = 1,
    Trial = 2,
    RollbackPending = 3,
}

/// <summary>一个外部固件槽的 Header 和全量校验结果。</summary>
public sealed class FirmwareSlotInfo
{
    public bool HeaderValid { get; init; }
    public bool FullValid { get; init; }
    public uint DataLength { get; init; }
    public uint DataCrc32 { get; init; }
    public string Version { get; init; } = string.Empty;
}

/// <summary>Bootloader 返回的 BCR 及两个外部槽状态。</summary>
public sealed class BootStatus
{
    public bool StateFound { get; init; }
    public BootPhase Phase { get; init; }
    public byte ActiveSlot { get; init; }
    public byte PendingSlot { get; init; }
    public byte AttemptsLeft { get; init; }
    public uint Sequence { get; init; }
    public uint ActiveCrc32 { get; init; }
    public uint PendingCrc32 { get; init; }
    public bool InternalValid { get; init; }
    public FirmwareSlotInfo[] Slots { get; init; } = [];
}

/// <summary>设备当前执行的防降级策略状态。</summary>
public sealed class FirmwareSecurityStatus
{
    public uint MinimumVersion { get; init; }
    public uint PendingVersion { get; init; }
    public bool DowngradeAuthorized { get; init; }
}

/// <summary>
/// USART3 Bootloader 协议客户端。普通命令执行同步的请求/响应事务；固件中间块
/// 使用接受 ACK 形成收写流水线，最终同步块和 SLOT_ACTIVATE 完成写入及 A/B 提交。
/// </summary>
public sealed class Transfer : IDisposable
{
    /* STM32F407 的 42 MHz APB1 时钟可以无误差产生 2 Mbaud。 */
    public const int UpgradeBaudRate = 2_000_000;

    private const byte PacketRequestHeader = 0xAA;
    private const byte PacketResponseHeader = 0x55;
    private const int DefaultBaudRate = 115200;

    private const int PacketCrcSize = 2;
    private const int PacketHeaderOffset = 0;
    private const int PacketOpcodeOffset = 1;
    private const int PacketErrorCodeOffset = 2;
    private const int PacketLengthOffset = 3;
    private const int PacketPayloadOffset = 5;

    private const int DefaultTimeoutSeconds = 1;
    private const int W25ProgramParameterHeaderSize = 7;
    private const int W25ProgramDataMax = 8192;
    private const int ResumeBlockSize = 4096;
    private const uint ResumeEraseAlignment = 32u * 1024u;

    private readonly SerialPort port;
    private readonly Channel<byte> dataChannel;

    public event Action<double>? OnProgress;
    public event Action<string>? OnEvent;

    private enum ResponseState
    {
        Header,
        Opcode,
        ErrorCode,
        Length,
        Payload,
        Crc16,
    }

    private enum Opcode : byte
    {
        Inquiry = 0x01,
        SetBaud = 0x02,
        Reset = 0x21,
        Boot = 0x22,
        W25Read = 0x41,
        W25Erase = 0x42,
        W25Program = 0x43,
        W25ProgramAsync = 0x44,
        SlotStatus = 0x50,
        SlotActivate = 0x52,
        SlotStatusFast = 0x54,
        SecurityStatus = 0x55,
        SlotResumeScan = 0x56,
    }

    private enum Inquiry : byte
    {
        GetVersion = 0,
        GetMtu = 1,
    }

    private enum ErrorCode : byte
    {
        Ok = 0,
        Opcode,
        Overflow,
        Timeout,
        Format,
        Verify,
        Parameter,
        Denied,
        Unknown = 0xFF,
    }

    private sealed record Response(ErrorCode ErrorCode, byte[] Payload);

    private readonly record struct ResumeScanResult(bool Success, uint Offset);

    public Transfer(string portName)
    {
        port = new SerialPort
        {
            PortName = portName,
            BaudRate = DefaultBaudRate,
            /* 为单个 8 KiB 请求及 USB 驱动分段预留充足的主机发送缓冲。 */
            WriteBufferSize = 32 * 1024,
        };
        port.DataReceived += OnDataReceived;
        dataChannel = Channel.CreateUnbounded<byte>();
    }

    /// <summary>
    /// 打开一次串口会话。升级过程保持同一连接，避免每个 4 KiB 数据包都让
    /// Windows 和 USB-TTL 重新打开设备。
    /// </summary>
    public bool Open()
    {
        if (port.IsOpen)
            return true;

        try
        {
            port.Open();
            return true;
        }
        catch (Exception ex)
        {
            OnEvent?.Invoke($"打开串口失败: {ex.Message}");
            return false;
        }
    }

    /// <summary>关闭串口并释放系统串口句柄。</summary>
    public void Dispose()
    {
        try
        {
            if (port.IsOpen)
                port.Close();
        }
        finally
        {
            port.Dispose();
        }
    }

    private void OnDataReceived(object sender, SerialDataReceivedEventArgs e)
    {
        int available = port.BytesToRead;
        if (available == 0)
            return;

        byte[] received = new byte[available];
        int count = port.Read(received, 0, received.Length);
        for (int index = 0; index < count; index++)
            dataChannel.Writer.TryWrite(received[index]);
    }

    /// <summary>从串口字节流中接收并校验指定 Opcode 的一帧响应。</summary>
    private async Task<Response?> WaitResponse(Opcode opcode, TimeSpan timeout)
    {
        ErrorCode errorCode = ErrorCode.Unknown;
        List<byte> frame = [];
        ResponseState state = ResponseState.Header;
        int payloadLength = 0;
        using CancellationTokenSource timeoutSource = new(timeout);

        try
        {
            await foreach (byte value in dataChannel.Reader.ReadAllAsync(timeoutSource.Token))
            {
                Trace.WriteLine($"recv: {value:X2}");
                frame.Add(value);
                switch (state)
                {
                    case ResponseState.Header:
                        if (frame[PacketHeaderOffset] == PacketResponseHeader)
                        {
                            state = ResponseState.Opcode;
                        }
                        else
                        {
                            frame.Clear();
                        }
                        break;

                    case ResponseState.Opcode:
                        if (frame[PacketOpcodeOffset] == (byte)opcode)
                        {
                            state = ResponseState.ErrorCode;
                        }
                        else
                        {
                            OnEvent?.Invoke(
                                $"opcode invalid: {frame[PacketOpcodeOffset]:X2}");
                            frame.Clear();
                            state = ResponseState.Header;
                        }
                        break;

                    case ResponseState.ErrorCode:
                        errorCode = (ErrorCode)frame[PacketErrorCodeOffset];
                        state = ResponseState.Length;
                        break;

                    case ResponseState.Length:
                        if (frame.Count == PacketPayloadOffset)
                        {
                            payloadLength = frame[PacketLengthOffset] |
                                            frame[PacketLengthOffset + 1] << 8;
                            state = payloadLength > 0
                                ? ResponseState.Payload
                                : ResponseState.Crc16;
                        }
                        break;

                    case ResponseState.Payload:
                        if (frame.Count == PacketPayloadOffset + payloadLength)
                            state = ResponseState.Crc16;
                        break;

                    case ResponseState.Crc16:
                        if (frame.Count == PacketPayloadOffset + payloadLength + PacketCrcSize)
                        {
                            byte[] packet = frame.ToArray();
                            ushort expectedCrc = BinaryPrimitives.ReadUInt16LittleEndian(
                                packet.AsSpan(PacketPayloadOffset + payloadLength));
                            ushort actualCrc = CRC.CRC16(packet.AsSpan(0,
                                packet.Length - PacketCrcSize));
                            if (actualCrc != expectedCrc)
                            {
                                OnEvent?.Invoke(
                                    $"crc16 error: expected {expectedCrc:X4}, got {actualCrc:X4}");
                                return null;
                            }

                            byte[] payload = packet
                                .AsSpan(PacketPayloadOffset, payloadLength)
                                .ToArray();
                            Trace.WriteLine(
                                $"packet received: opcode={opcode}, length={payloadLength}");
                            return new Response(errorCode, payload);
                        }
                        break;
                }
            }
        }
        catch (OperationCanceledException)
        {
            OnEvent?.Invoke("WaitResponse timeout");
        }
        catch (Exception ex)
        {
            OnEvent?.Invoke($"WaitResponse exception: {ex.Message}");
        }

        return null;
    }

    /// <summary>按小端协议布局构造一帧请求。</summary>
    private static byte[] BuildPacket(Opcode opcode, byte[]? payload)
    {
        int payloadLength = payload?.Length ?? 0;
        byte[] packet = new byte[PacketPayloadOffset + payloadLength + PacketCrcSize];

        packet[PacketHeaderOffset] = PacketRequestHeader;
        packet[PacketOpcodeOffset] = (byte)opcode;
        BinaryPrimitives.WriteUInt16LittleEndian(
            packet.AsSpan(PacketLengthOffset), checked((ushort)payloadLength));
        payload?.CopyTo(packet, PacketPayloadOffset);

        ushort crc = CRC.CRC16(packet.AsSpan(0, packet.Length - PacketCrcSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(packet.Length - PacketCrcSize), crc);
        return packet;
    }

    /// <summary>发送一帧请求，并等待匹配 Opcode 的完整响应。</summary>
    private async Task<Response?> PacketRequest(Opcode opcode,
                                                byte[]? payload,
                                                TimeSpan timeout = default)
    {
        bool openedHere = false;

        if (timeout == default)
            timeout = TimeSpan.FromSeconds(DefaultTimeoutSeconds);

        try
        {
            byte[] packet = BuildPacket(opcode, payload);
            while (dataChannel.Reader.TryRead(out _))
            {
            }
            if (!port.IsOpen)
            {
                if (!Open())
                    return null;
                openedHere = true;
            }
            port.Write(packet, 0, packet.Length);
            return await WaitResponse(opcode, timeout);
        }
        catch (Exception ex)
        {
            OnEvent?.Invoke($"PacketRequest exception: {ex.Message}");
            return null;
        }
        finally
        {
            try
            {
                if (openedHere && port.IsOpen)
                    port.Close();
            }
            catch (Exception ex)
            {
                OnEvent?.Invoke($"Failed to close port: {ex.Message}");
            }
        }
    }

    /// <summary>查询 Bootloader 版本字符串。</summary>
    public async Task<string?> GetBootloaderVersion()
    {
        Response? response = await PacketRequest(Opcode.Inquiry, [(byte)Inquiry.GetVersion]);

        return response?.ErrorCode == ErrorCode.Ok
            ? Encoding.ASCII.GetString(response.Payload)
            : null;
    }

    /// <summary>查询请求帧允许的最大 Payload 长度。</summary>
    public async Task<int?> GetBootloaderMTU()
    {
        Response? response = await PacketRequest(Opcode.Inquiry, [(byte)Inquiry.GetMtu]);

        return response?.ErrorCode == ErrorCode.Ok && response.Payload.Length == sizeof(ushort)
            ? BinaryPrimitives.ReadUInt16LittleEndian(response.Payload)
            : null;
    }

    /// <summary>
    /// 在 115200 默认链路收到 Bootloader ACK 后，把当前会话切到更高波特率。
    /// 切换后立即查询一次版本，确认 PC 串口和 MCU 确实工作在同一速率。
    /// </summary>
    public async Task<bool> SetBaudRate(int baudRate)
    {
        if (baudRate <= 0)
        {
            OnEvent?.Invoke("波特率必须大于 0");
            return false;
        }

        if (baudRate == DefaultBaudRate)
            return true;

        byte[] payload = new byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32LittleEndian(payload, checked((uint)baudRate));
        Response? response = await PacketRequest(Opcode.SetBaud, payload);
        if (response?.ErrorCode != ErrorCode.Ok)
        {
            OnEvent?.Invoke($"Bootloader不支持切换到 {baudRate} baud");
            return false;
        }

        try
        {
            port.BaudRate = baudRate;
            while (dataChannel.Reader.TryRead(out _))
            {
            }
            port.DiscardInBuffer();
        }
        catch (Exception ex)
        {
            OnEvent?.Invoke($"PC串口切换波特率失败: {ex.Message}");
            return false;
        }

        string? version = await GetBootloaderVersion();
        if (version == null)
        {
            OnEvent?.Invoke("高速链路复核失败；请复位设备后从115200重新连接");
            return false;
        }

        OnEvent?.Invoke($"协议链路已切换到 {baudRate} baud");
        return true;
    }

    /// <summary>
    /// 使用一次最小的 W25Q 只读请求探测工程维护接口是否开放。
    /// true 表示允许，false 表示 Bootloader 明确返回 DENIED，null 表示链路或协议异常。
    /// 此方法只读取 A 槽首字节，不会擦除或改写任何存储内容。
    /// </summary>
    public async Task<bool?> IsMaintenanceReadAllowed()
    {
        byte[] payload = new byte[7];
        payload[0] = 0; // W25Q 芯片 0（A 槽所在芯片）
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(1), 0u);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(5), 1);

        Response? response = await PacketRequest(Opcode.W25Read, payload,
                                                 TimeSpan.FromSeconds(2));
        if (response?.ErrorCode == ErrorCode.Ok)
            return true;
        if (response?.ErrorCode == ErrorCode.Denied)
            return false;
        return null;
    }

    /// <summary>读取 BCR 以及两个外部槽的 Header 和全量校验结果。</summary>
    public async Task<BootStatus?> GetBootStatus()
    {
        Response? response = await PacketRequest(
            Opcode.SlotStatus, null, TimeSpan.FromSeconds(15));
        byte[]? data = response?.Payload;
        if (response?.ErrorCode != ErrorCode.Ok || data?.Length != 102)
            return null;

        FirmwareSlotInfo[] slots = new FirmwareSlotInfo[2];
        for (int slot = 0; slot < 2; ++slot)
        {
            int offset = 17 + slot * 42;
            slots[slot] = new FirmwareSlotInfo
            {
                HeaderValid = data[offset] != 0,
                FullValid = data[offset + 1] != 0,
                DataLength = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(offset + 2)),
                DataCrc32 = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(offset + 6)),
                Version = Encoding.ASCII.GetString(data, offset + 10, 32).TrimEnd('\0'),
            };
        }

        return new BootStatus
        {
            StateFound = data[0] != 0,
            Phase = (BootPhase)data[1],
            ActiveSlot = data[2],
            PendingSlot = data[3],
            AttemptsLeft = data[4],
            Sequence = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(5)),
            ActiveCrc32 = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(9)),
            PendingCrc32 = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(13)),
            InternalValid = data[101] != 0,
            Slots = slots,
        };
    }

    /// <summary>
    /// 只读取经过 EEPROM 双副本及 CRC 校验的 BCR 状态，不扫描 A/B 槽的数据区。
    /// Formal 升级流程依赖该命令；返回 null 时停止升级，不擦写目标槽。
    /// 固件是否可安装仍由 SLOT_ACTIVATE 的全量 CRC 和安全校验决定。
    /// </summary>
    private async Task<BootStatus?> GetBootStatusFast()
    {
        Response? response = await PacketRequest(Opcode.SlotStatusFast, null,
                                                 TimeSpan.FromSeconds(2));
        byte[]? data = response?.Payload;
        if (response?.ErrorCode != ErrorCode.Ok || data?.Length != 17)
            return null;

        return new BootStatus
        {
            StateFound = data[0] != 0,
            Phase = (BootPhase)data[1],
            ActiveSlot = data[2],
            PendingSlot = data[3],
            AttemptsLeft = data[4],
            Sequence = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(5)),
            ActiveCrc32 = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(9)),
            PendingCrc32 = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(13)),
            Slots = [],
        };
    }

    /// <summary>
    /// 查询设备防降级下限。返回 null 表示链路或 Formal 协议异常。
    /// </summary>
    public async Task<FirmwareSecurityStatus?> GetFirmwareSecurityStatus()
    {
        Response? response = await PacketRequest(Opcode.SecurityStatus, null,
                                                 TimeSpan.FromSeconds(2));
        byte[]? data = response?.Payload;
        if (response?.ErrorCode != ErrorCode.Ok || data?.Length != 9)
            return null;

        return new FirmwareSecurityStatus
        {
            MinimumVersion = BinaryPrimitives.ReadUInt32LittleEndian(data),
            PendingVersion = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(4)),
            DowngradeAuthorized = data[8] != 0,
        };
    }

    /// <summary>
    /// 构造断点扫描请求。CRC 列表只覆盖应用数据区，每个元素对应一个 4 KiB 块，
    /// 最后一块按实际数据长度计算，不包含扇区尾部的擦除填充。
    /// </summary>
    private static byte[] BuildResumeScanRequest(byte slot, byte[] xbin,
                                                  MagicHeader header)
    {
        int dataOffset = checked((int)header.DataOffset);
        int dataLength = checked((int)header.DataLength);
        int blockCount = (dataLength + ResumeBlockSize - 1) /
                         ResumeBlockSize;
        byte[] payload = new byte[7 + blockCount * sizeof(uint)];

        payload[0] = slot;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(1), header.DataLength);
        BinaryPrimitives.WriteUInt16LittleEndian(
            payload.AsSpan(5), checked((ushort)blockCount));
        for (int block = 0; block < blockCount; ++block)
        {
            int offset = block * ResumeBlockSize;
            int size = Math.Min(ResumeBlockSize, dataLength - offset);
            uint crc = CRC.CRC32(xbin.AsSpan(dataOffset + offset, size));
            BinaryPrimitives.WriteUInt32LittleEndian(
                payload.AsSpan(7 + block * sizeof(uint)), crc);
        }
        return payload;
    }

    /// <summary>
    /// 让 Bootloader 使非活动槽 Header 失效，并返回与当前 XBIN 连续匹配的数据末端。
    /// 命令失败时直接终止升级，不进入擦除阶段。
    /// </summary>
    private async Task<ResumeScanResult> ScanResumeOffset(byte slot, byte[] xbin,
                                                           MagicHeader header)
    {
        byte[] payload = BuildResumeScanRequest(slot, xbin, header);
        Response? response = await PacketRequest(Opcode.SlotResumeScan, payload,
                                                 TimeSpan.FromSeconds(5));
        if (response == null)
            return new ResumeScanResult(false, 0);
        if (response.ErrorCode != ErrorCode.Ok || response.Payload.Length != 4)
            return new ResumeScanResult(false, 0);

        uint offset = BinaryPrimitives.ReadUInt32LittleEndian(response.Payload);
        uint dataEnd = header.DataOffset + header.DataLength;
        bool alignedBlock = offset >= header.DataOffset && offset < dataEnd &&
                            (offset - header.DataOffset) % ResumeBlockSize == 0;
        if (offset != dataEnd && !alignedBlock)
            return new ResumeScanResult(false, 0);

        return new ResumeScanResult(true, offset);
    }

    private async Task<bool> W25Erase(byte slot, uint address, uint size)
    {
        byte[] payload = new byte[9];
        payload[0] = slot;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(1), address);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(5), size);
        Response? response = await PacketRequest(Opcode.W25Erase, payload,
                                                 TimeSpan.FromSeconds(20));
        return response?.ErrorCode == ErrorCode.Ok;
    }

    private async Task<bool> W25Program(byte slot, uint address, byte[] data, int mtu,
                                        int progressBase, int progressTotal)
    {
        int offset = 0;
        int chunkMax = Math.Min(W25ProgramDataMax,
                                mtu - W25ProgramParameterHeaderSize);

        if (chunkMax <= 0)
            return false;

        while (offset < data.Length)
        {
            int chunk = Math.Min(chunkMax, data.Length - offset);
            byte[] payload = new byte[W25ProgramParameterHeaderSize + chunk];
            payload[0] = slot;
            BinaryPrimitives.WriteUInt32LittleEndian(
                payload.AsSpan(1), address + (uint)offset);
            BinaryPrimitives.WriteUInt16LittleEndian(
                payload.AsSpan(5), checked((ushort)chunk));
            Array.Copy(data, offset, payload, W25ProgramParameterHeaderSize, chunk);

            /*
             * 中间块收到的是“CRC和参数已接受”ACK：MCU随后写当前块，PC同时
             * 发送下一块。最后一块改用同步命令，等所有实际写入完成后才返回。
             */
            Opcode opcode = offset + chunk < data.Length
                ? Opcode.W25ProgramAsync
                : Opcode.W25Program;
            Response? response = await PacketRequest(opcode, payload,
                                                     TimeSpan.FromSeconds(5));
            if (response?.ErrorCode != ErrorCode.Ok)
                return false;

            offset += chunk;
            OnProgress?.Invoke((double)(progressBase + offset) /
                               progressTotal * 100.0);
        }
        return true;
    }

    private async Task<bool> ActivateSlot(byte slot, uint expectedDataCrc)
    {
        byte[] payload = new byte[5];
        payload[0] = slot;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(1), expectedDataCrc);

        Response? response = await PacketRequest(Opcode.SlotActivate, payload,
                                                 TimeSpan.FromSeconds(15));
        if (response?.ErrorCode == ErrorCode.Denied)
            OnEvent?.Invoke("Bootloader拒绝安装：固件安全版本低于设备下限，需使用新签名回退包或KEY2授权");
        return response?.ErrorCode == ErrorCode.Ok;
    }

    /// <summary>
    /// 把XBIN写入非活动槽，完成签名校验后提交为InstallPending。
    ///
    /// 默认先使用4 KiB分块CRC查找已经落盘的连续前缀，只擦写首个不一致块
    /// 之后的区域。Formal Bootloader必须支持该命令，不再回退旧协议。
    /// 无论采用哪条路径，Header都最后写入，提交前不会改写内部应用或活动槽。
    /// </summary>
    /// <param name="forceFull">true时跳过断点扫描并完整擦写目标槽。</param>
    public async Task<bool> UpgradeExternalAB(byte[] xbin, int mtu,
                                              MagicHeader header,
                                              bool forceFull = false)
    {
        Stopwatch totalTimer = Stopwatch.StartNew();
        Stopwatch stepTimer = Stopwatch.StartNew();
        BootStatus? status = await GetBootStatusFast();
        OnEvent?.Invoke($"读取A/B状态耗时: {stepTimer.Elapsed.TotalSeconds:F2} 秒");
        if (status == null)
        {
            OnEvent?.Invoke("无法读取A/B状态");
            return false;
        }

        if (!status.StateFound || status.ActiveSlot >= 2)
        {
            OnEvent?.Invoke("A/B尚未初始化：请先预置与内部Formal固件匹配的外部槽并重启采用");
            return false;
        }

        if (status.Phase != BootPhase.Normal)
        {
            OnEvent?.Invoke($"当前A/B阶段为 {status.Phase}，请先完成试运行或回退");
            return false;
        }

        byte target = (byte)(status.ActiveSlot ^ 1);
        uint eraseSize = (uint)((xbin.Length + 4095) & ~4095);
        bool resumable = false;
        uint resumeOffset = header.DataOffset;
        OnEvent?.Invoke($"活动槽: {(char)('A' + status.ActiveSlot)}，写入非活动槽: {(char)('A' + target)}");

        if (!forceFull)
        {
            stepTimer.Restart();
            ResumeScanResult scan = await ScanResumeOffset(target, xbin, header);
            if (!scan.Success)
            {
                OnEvent?.Invoke("断点扫描失败，未擦写活动固件");
                return false;
            }

            resumable = true;
            resumeOffset = scan.Offset;
            OnEvent?.Invoke($"断点扫描耗时: {stepTimer.Elapsed.TotalSeconds:F2} 秒");
        }
        else
        {
            OnEvent?.Invoke("已选择强制完整重写，跳过断点扫描");
        }

        stepTimer.Restart();
        if (!resumable)
        {
            OnEvent?.Invoke($"擦除外部槽区: {eraseSize} 字节");
            if (!await W25Erase(target, 0, eraseSize))
                return false;

            int headerCommitSize = header.HeaderCommitSize;
            byte[] body = xbin[headerCommitSize..];
            if (!await W25Program(target, (uint)headerCommitSize, body, mtu,
                                  0, xbin.Length) ||
                !await W25Program(target, 0, xbin[..headerCommitSize], mtu,
                                  body.Length, xbin.Length))
                return false;
        }
        else
        {
            uint dataEnd = header.DataOffset + header.DataLength;
            int headerCommitSize = header.HeaderCommitSize;
            int progressTotal = checked((int)header.DataLength + headerCommitSize);
            bool headerSectorErased = false;

            if (resumeOffset < dataEnd)
            {
                /*
                 * 从前一个32 KiB边界重写少量已匹配数据，使W25Q驱动可以优先
                 * 使用Block Erase，避免从非对齐地址连续执行大量4 KiB擦除。
                 */
                uint eraseStart = resumeOffset & ~(ResumeEraseAlignment - 1u);
                uint writeStart = Math.Max(eraseStart, header.DataOffset);
                uint eraseEnd = (dataEnd + ResumeBlockSize - 1u) &
                                ~(uint)(ResumeBlockSize - 1);
                uint preserved = writeStart - header.DataOffset;

                OnEvent?.Invoke($"已复用 {preserved} 字节，从槽偏移0x{writeStart:X}继续");
                if (!await W25Erase(target, eraseStart, eraseEnd - eraseStart))
                    return false;
                headerSectorErased = eraseStart == 0;

                byte[] remaining = xbin[(int)writeStart..(int)dataEnd];
                if (!await W25Program(target, writeStart, remaining, mtu,
                                      checked((int)preserved), progressTotal))
                    return false;
            }
            else
            {
                OnEvent?.Invoke($"应用数据已完整存在，复用全部 {header.DataLength} 字节");
                OnProgress?.Invoke((double)header.DataLength / progressTotal * 100.0);
            }

            /* 扫描命令已把magic清零；真正提交Header前确保整个首扇区已擦除。 */
            if (!headerSectorErased &&
                !await W25Erase(target, 0, ResumeBlockSize))
                return false;
            if (!await W25Program(target, 0, xbin[..headerCommitSize], mtu,
                                  checked((int)header.DataLength), progressTotal))
                return false;
        }
        OnEvent?.Invoke($"串口传输及W25编程耗时: {stepTimer.Elapsed.TotalSeconds:F2} 秒");

        OnEvent?.Invoke("外部写入完成，正在由Bootloader校验完整固件并提交试运行");
        stepTimer.Restart();
        if (!await ActivateSlot(target, header.DataCrc32))
            return false;
        OnEvent?.Invoke($"完整校验及提交耗时: {stepTimer.Elapsed.TotalSeconds:F2} 秒");

        OnProgress?.Invoke(100);
        OnEvent?.Invoke($"外部A/B升级总耗时: {totalTimer.Elapsed.TotalSeconds:F2} 秒");
        return true;
    }

    /// <summary>请求MCU立即复位。</summary>
    public async Task<bool> Reset()
    {
        Response? response = await PacketRequest(
            Opcode.Reset, null, TimeSpan.FromSeconds(DefaultTimeoutSeconds));
        return response?.ErrorCode == ErrorCode.Ok;
    }

    /// <summary>请求Bootloader通过统一A/B状态机准备并启动应用。</summary>
    public async Task<bool> Boot()
    {
        Response? response = await PacketRequest(
            Opcode.Boot, null, TimeSpan.FromSeconds(DefaultTimeoutSeconds));
        return response?.ErrorCode == ErrorCode.Ok;
    }
}
