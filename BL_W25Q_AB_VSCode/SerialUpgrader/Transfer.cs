using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using System.Threading.Channels;
using Timer = System.Timers.Timer;

namespace SerialUpgrader;

/// <summary>Bootloader持久化的A/B启动阶段。</summary>
public enum BootPhase : byte
{
    Normal = 0,
    InstallPending = 1,
    Trial = 2,
    RollbackPending = 3,
}

/// <summary>一个外部固件槽的Header和全量校验结果。</summary>
public sealed class FirmwareSlotInfo
{
    public bool HeaderValid { get; init; }
    public bool FullValid { get; init; }
    public uint DataLength { get; init; }
    public uint DataCrc32 { get; init; }
    public string Version { get; init; } = string.Empty;
}

/// <summary>Bootloader返回的BCR及两个外部槽状态。</summary>
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
/// USART3 Bootloader协议客户端。普通请求执行同步的请求/响应事务；固件中间块
/// 使用接受ACK形成收写流水线，最终同步块和SLOT_ACTIVATE完成写入及A/B提交。
/// </summary>
public sealed class Transfer : IDisposable
{
    /* STM32F407的42 MHz APB1时钟可以无误差产生2 Mbaud。 */
    public const int UpgradeBaudRate = 2_000_000;

    // 协议常数
    private const byte PACKET_HEADER_REQUEST = 0xAA;
    private const byte PACKET_HEADER_RESPONSE = 0x55;
    private const int DEFAULT_BAUD_RATE = 115200;
    
    // 数据包结构常数
    private const int PACKET_HEADER_SIZE = 1;
    private const int PACKET_OPCODE_SIZE = 1;
    private const int PACKET_LENGTH_SIZE = 2;
    private const int PACKET_CRC_SIZE = 2;
    private const int PACKET_HEADER_OFFSET = 0;
    private const int PACKET_OPCODE_OFFSET = 1;
    private const int PACKET_ERRCODE_OFFSET = 2;
    private const int PACKET_LENGTH_OFFSET = 3;
    private const int PACKET_PAYLOAD_OFFSET = 5;
    private const int PACKET_MIN_SIZE = PACKET_HEADER_SIZE + PACKET_OPCODE_SIZE + PACKET_LENGTH_SIZE + PACKET_CRC_SIZE;
    
    // 超时配置常数
    private const int DEFAULT_TIMEOUT_SECONDS = 1;
    private const int ERASE_TIMEOUT_SECONDS = 10;
    private const int PROGRAM_TIMEOUT_SECONDS = 10;
    private const int VERIFY_TIMEOUT_SECONDS = 10;
    
    // 参数长度常数
    private const int ADDR_SIZE_PARAM_LENGTH = 8;  // uint addr + uint size
    private const int ADDR_SIZE_CRC_PARAM_LENGTH = 12;  // uint addr + uint size + uint crc
    private const int PROGRAM_PARAM_HEADER_SIZE = 8;  // uint addr + uint size
    private const int W25_PROGRAM_PARAM_HEADER_SIZE = 7;
    private const int W25_PROGRAM_DATA_MAX = 8192;
    private const int RESUME_BLOCK_SIZE = 4096;
    private const uint RESUME_ERASE_ALIGNMENT = 32 * 1024;
    
    public Action<double>? OnProgress;
    public Action<string>? OnEvent;
    private SerialPort Port;
    private Channel<byte> DataChannel;

    enum ResponseState
    {
        HEADER,
        OPCODE,
        ERRCODE,
        LENGTH,
        PAYLOAD,
        CRC16,
    }

    enum OPCODE
    {
        INQUERY = 0x01,
        SET_BAUD = 0x02,
        ERASE = 0x81,
        PROGRAM = 0x82,
        VERIFY = 0x83,
        RESET = 0x21,
        BOOT = 0x22,
        W25_READ = 0x41,
        W25_ERASE = 0x42,
        W25_PROGRAM = 0x43,
        W25_PROGRAM_ASYNC = 0x44,
        SLOT_STATUS = 0x50,
        SLOT_ACTIVATE = 0x52,
        SLOT_SNAPSHOT = 0x53,
        SLOT_STATUS_FAST = 0x54,
        SECURITY_STATUS = 0x55,
        SLOT_RESUME_SCAN = 0x56,
    }

    enum INQUIRY
    {
        GET_VERSION = 0,
        GET_MTU = 1,
    }
    enum ERRCODE
{
        OK = 0,
        OPCODE,
        OVERFLOW,
        TIMEOUT,
        FORMAT,
        VERIFY,
        PARAM,
        DENIED,
        UNKNOWN = 0xff,
    }

    class Response
    {
        public OPCODE? Opcode;
        public ERRCODE? Errcode;
        public byte[]? Param;

        public Response(OPCODE? opcode, ERRCODE? errcode, byte[]? param)
        {
            Opcode = opcode;
            Errcode = errcode;
            Param = param;
        }
    }

    public Transfer(string port)
    {
        Port = new();
        Port.PortName = port;
        Port.BaudRate = DEFAULT_BAUD_RATE;
        /* 为单个8 KiB请求及USB驱动分段预留充足的主机发送缓冲。 */
        Port.WriteBufferSize = 32 * 1024;
        Port.DataReceived += OnDataReceived;

        DataChannel = Channel.CreateUnbounded<byte>();
    }

    readonly record struct ResumeScanResult(bool Supported, bool Success,
                                            uint Offset);

    /// <summary>
    /// 打开一次串口会话。升级过程保持同一连接，避免每个4 KiB数据包都让
    /// Windows和USB-TTL重新打开设备。
    /// </summary>
    public bool Open()
    {
        if (Port.IsOpen)
            return true;

        try
        {
            Port.Open();
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
            if (Port.IsOpen)
                Port.Close();
        }
        finally
        {
            Port.Dispose();
        }
    }

    void OnDataReceived(object sender, SerialDataReceivedEventArgs e)
    {
        byte[] recv = new byte[Port.BytesToRead];
        Port.Read(recv, 0, recv.Length);
        foreach (byte b in recv) DataChannel.Writer.TryWrite(b);
    }

    async Task<Response?> WaitResponse(OPCODE opcode, TimeSpan timeout)
    {
        ERRCODE errcode = ERRCODE.UNKNOWN;
        byte[]? param = null;

        List<byte> response = [];
        ResponseState state = ResponseState.HEADER;
        int payload_length = 0;

        CancellationTokenSource cancelTokenSource = new();
        Timer timeouter = new Timer();
        timeouter.Interval = timeout.TotalMilliseconds;
        timeouter.AutoReset = false;
        timeouter.Elapsed += (sender, e) =>
        {
            cancelTokenSource.Cancel();
        };
        timeouter.Start();

        DateTime last_rx_time = DateTime.Now;

        try
        {
            await foreach (byte data in DataChannel.Reader.ReadAllAsync(cancelTokenSource.Token))
            {
                if (DateTime.Now - last_rx_time > timeout)
                {
                    if (response.Count > 0)
                        OnEvent?.Invoke("rx timeout, reset state machine");
                    response.Clear();
                    state = ResponseState.HEADER;
                }
                last_rx_time = DateTime.Now;

                Trace.WriteLine($"recv: {data:X2}");

                // 字节接收状态机处理
                response.Add(data);
                switch (state)
                {
                    case ResponseState.HEADER:
                        if (response[PACKET_HEADER_OFFSET] == PACKET_HEADER_RESPONSE)
                        {
                            Trace.WriteLine("header ok");
                            state = ResponseState.OPCODE;
                        }
                        else
                        {
                            response.Clear();
                            state = ResponseState.HEADER;
                        }
                        break;
                    case ResponseState.OPCODE:
                        if (response[PACKET_OPCODE_OFFSET] == (byte)opcode)
                        {
                            Trace.WriteLine($"opcode ok: {opcode}");
                            state = ResponseState.ERRCODE;
                        }
                        else
                        {
                            OnEvent?.Invoke($"opcode invalid: {response[PACKET_OPCODE_OFFSET]:X2}");
                            response.Clear();
                            state = ResponseState.HEADER;
                        }
                        break;
                    case ResponseState.ERRCODE:
                        {
                            errcode = (ERRCODE)response[PACKET_ERRCODE_OFFSET];
                            Trace.WriteLine($"errcode ok: {errcode}");
                            state = ResponseState.LENGTH;
                        }
                        break;
                    case ResponseState.LENGTH:
                        if (response.Count == PACKET_PAYLOAD_OFFSET)
                        {
                            payload_length = BitConverter.ToUInt16(response.ToArray(), PACKET_LENGTH_OFFSET);
                            Trace.WriteLine($"length ok: {payload_length}");
                            if (payload_length > 0)
                                state = ResponseState.PAYLOAD;
                            else
                                state = ResponseState.CRC16;
                        }
                        break;
                    case ResponseState.PAYLOAD:
                        if (response.Count == PACKET_PAYLOAD_OFFSET + payload_length)
                        {
                            Trace.WriteLine("payload ok");
                            state = ResponseState.CRC16;
                        }
                        break;
                    case ResponseState.CRC16:
                        if (response.Count == PACKET_PAYLOAD_OFFSET + payload_length + PACKET_CRC_SIZE)
                        {
                            ushort crc = BitConverter.ToUInt16(response.ToArray(), PACKET_PAYLOAD_OFFSET + payload_length);
                            ushort ccrc = CRC.CRC16(response.Take(response.Count - PACKET_CRC_SIZE).ToArray());
                            if (ccrc == crc)
                            {
                                param = response.Skip(PACKET_PAYLOAD_OFFSET).Take(payload_length).ToArray();
                                Trace.WriteLine($"crc16 ok: {crc:X4}");
                                Trace.WriteLine($"packet received: opcode={opcode}, length={payload_length}");
                                return new Response(opcode, errcode, param);
                            }
                            else
                            {
                                OnEvent?.Invoke($"crc16 error: expected {crc:X4}, got {ccrc:X4}");
                                return null;
                            }
                        }
                        break;
                    default:
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
        finally
        {
            timeouter.Stop();
            timeouter.Dispose();
            cancelTokenSource.Dispose();
        }

        return null;
    }

    byte[] BuildPacket(OPCODE opcode, byte[]? param)
    {
        List<byte> packet = [];
        packet.Add(PACKET_HEADER_REQUEST);
        packet.Add((byte)opcode);
        packet.AddRange(BitConverter.GetBytes((ushort)(param?.Length ?? 0)));
        if (param != null) packet.AddRange(param);
        ushort crc = CRC.CRC16(packet.ToArray());
        packet.AddRange(BitConverter.GetBytes(crc));

        return packet.ToArray();
    }

    async Task<Response?> PacketRequest(OPCODE opcode, byte[]? param, TimeSpan timeout = default)
    {
        bool openedHere = false;

        if (timeout == default)
            timeout = TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS);

        try
        {
            byte[] packet = BuildPacket(opcode, param);
            while (DataChannel.Reader.TryRead(out _)) { }
            if (!Port.IsOpen)
            {
                if (!Open())
                    return null;
                openedHere = true;
            }
            Port.Write(packet, 0, packet.Length);
            Response? response = await WaitResponse(opcode, timeout);
            return response;
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
                if (openedHere && Port.IsOpen)
                    Port.Close();
            }
            catch (Exception ex)
            {
                OnEvent?.Invoke($"Failed to close port: {ex.Message}");
            }
        }
    }

    /// <summary>查询Bootloader版本字符串。</summary>
    public async Task<string?> GetBootloaderVersion()
    {
        Response? response = await PacketRequest(OPCODE.INQUERY, [(byte)INQUIRY.GET_VERSION]);
        
        if (response != null && response.Errcode == ERRCODE.OK && response.Param != null)
        {
            string version = Encoding.ASCII.GetString(response.Param);
            return version;
        }
        
        return null;
    }

    /// <summary>查询请求帧允许的最大Payload长度。</summary>
    public async Task<int?> GetBootloaderMTU()
    {
        Response? response = await PacketRequest(OPCODE.INQUERY, [(byte)INQUIRY.GET_MTU]);

        if (response != null && response.Errcode == ERRCODE.OK && response.Param != null)
        {
            int mtu = BitConverter.ToUInt16(response.Param);
            return mtu;
        }

        return null;
    }

    /// <summary>
    /// 在115200默认链路收到Bootloader ACK后，把当前会话切到更高波特率。
    /// 切换后立即查询一次版本，确认PC串口和MCU确实工作在同一速率。
    /// </summary>
    public async Task<bool> SetBaudRate(int baudRate)
    {
        if (baudRate == DEFAULT_BAUD_RATE)
            return true;

        Response? response = await PacketRequest(OPCODE.SET_BAUD,
                                                 BitConverter.GetBytes((uint)baudRate));
        if (response?.Errcode != ERRCODE.OK)
        {
            OnEvent?.Invoke($"Bootloader不支持切换到 {baudRate} baud");
            return false;
        }

        try
        {
            Port.BaudRate = baudRate;
            while (DataChannel.Reader.TryRead(out _)) { }
            Port.DiscardInBuffer();
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

    /// <summary>旧协议内部Flash擦除命令，仅用于兼容和维护。</summary>
    public async Task<bool> Erase(uint addr, uint size)
    {
        byte[] param = new byte[ADDR_SIZE_PARAM_LENGTH];
        BitConverter.GetBytes(addr).CopyTo(param, 0);
        BitConverter.GetBytes(size).CopyTo(param, 4);

        Response? response = await PacketRequest(OPCODE.ERASE, param, TimeSpan.FromSeconds(ERASE_TIMEOUT_SECONDS));

        if (response != null && response.Errcode == ERRCODE.OK)
        {
            return true;
        }

        return false;
    }

    /// <summary>旧协议内部Flash编程命令，仅用于兼容和维护。</summary>
    public async Task<bool> Program(uint addr, byte[] data, int mtu)
    {
        int offset = 0;
        while (offset < data.Length)
        {
            int chunk_size = Math.Min(mtu - PROGRAM_PARAM_HEADER_SIZE, data.Length - offset);
            OnProgress?.Invoke((double)offset / data.Length * 100.0);
            OnEvent?.Invoke($"Program addr:0x{addr:X8} size:{chunk_size} {offset}/{data.Length}");
            byte[] param = new byte[PROGRAM_PARAM_HEADER_SIZE + chunk_size];
            BitConverter.GetBytes(addr).CopyTo(param, 0);
            BitConverter.GetBytes((uint)chunk_size).CopyTo(param, 4);

            Array.Copy(data, offset, param, PROGRAM_PARAM_HEADER_SIZE, chunk_size);
            Response? response = await PacketRequest(OPCODE.PROGRAM, param, TimeSpan.FromSeconds(PROGRAM_TIMEOUT_SECONDS));
            if (response == null || response.Errcode != ERRCODE.OK)
            {
                return false;
            }
            addr += (uint)chunk_size;
            offset += chunk_size;
        }

        return true;
    }

    /// <summary>旧协议内部Flash CRC32校验命令，仅用于兼容和维护。</summary>
    public async Task<bool> Verify(uint addr, uint size, uint crc)
    {
        byte[] param = new byte[ADDR_SIZE_CRC_PARAM_LENGTH];
        BitConverter.GetBytes(addr).CopyTo(param, 0);
        BitConverter.GetBytes(size).CopyTo(param, 4);
        BitConverter.GetBytes(crc).CopyTo(param, 8);
        Response? response = await PacketRequest(OPCODE.VERIFY, param, TimeSpan.FromSeconds(VERIFY_TIMEOUT_SECONDS));
        if (response != null && response.Errcode == ERRCODE.OK)
        {
            return true;
        }
        return false;
    }

    /// <summary>
    /// 使用一次最小的W25Q只读请求探测工程维护接口是否开放。
    /// true表示允许，false表示Bootloader明确返回DENIED，null表示链路或协议异常。
    /// 此方法只读取A槽首字节，不会擦除或改写任何存储内容。
    /// </summary>
    public async Task<bool?> IsMaintenanceReadAllowed()
    {
        byte[] param = new byte[7];
        param[0] = 0;                         // W25Q芯片0（A槽所在芯片）
        BitConverter.GetBytes(0u).CopyTo(param, 1);
        BitConverter.GetBytes((ushort)1).CopyTo(param, 5);

        Response? response = await PacketRequest(OPCODE.W25_READ, param,
                                                 TimeSpan.FromSeconds(2));
        if (response?.Errcode == ERRCODE.OK)
            return true;
        if (response?.Errcode == ERRCODE.DENIED)
            return false;
        return null;
    }

    /// <summary>读取BCR以及两个外部槽的Header和全量校验结果。</summary>
    public async Task<BootStatus?> GetBootStatus()
    {
        Response? response = await PacketRequest(OPCODE.SLOT_STATUS, null, TimeSpan.FromSeconds(15));
        byte[]? data = response?.Param;
        if (response?.Errcode != ERRCODE.OK || data == null || data.Length != 101)
            return null;

        FirmwareSlotInfo[] slots = new FirmwareSlotInfo[2];
        for (int slot = 0; slot < 2; ++slot)
        {
            int offset = 17 + slot * 42;
            slots[slot] = new FirmwareSlotInfo
            {
                HeaderValid = data[offset] != 0,
                FullValid = data[offset + 1] != 0,
                DataLength = BitConverter.ToUInt32(data, offset + 2),
                DataCrc32 = BitConverter.ToUInt32(data, offset + 6),
                Version = Encoding.ASCII.GetString(data, offset + 10, 32).TrimEnd('\0')
            };
        }

        return new BootStatus
        {
            StateFound = data[0] != 0,
            Phase = (BootPhase)data[1],
            ActiveSlot = data[2],
            PendingSlot = data[3],
            AttemptsLeft = data[4],
            Sequence = BitConverter.ToUInt32(data, 5),
            ActiveCrc32 = BitConverter.ToUInt32(data, 9),
            PendingCrc32 = BitConverter.ToUInt32(data, 13),
            Slots = slots
        };
    }

    /// <summary>
    /// 只读取经过EEPROM双副本及CRC校验的BCR状态，不扫描A/B槽的数据区。
    /// 返回null表示当前Bootloader不支持该命令或链路异常；调用方可以回退到
    /// 完整GetBootStatus。固件是否可安装仍由SLOT_ACTIVATE的全量CRC决定。
    /// </summary>
    async Task<BootStatus?> GetBootStatusFast()
    {
        Response? response = await PacketRequest(OPCODE.SLOT_STATUS_FAST, null,
                                                 TimeSpan.FromSeconds(2));
        byte[]? data = response?.Param;
        if (response?.Errcode != ERRCODE.OK || data == null || data.Length != 17)
            return null;

        return new BootStatus
        {
            StateFound = data[0] != 0,
            Phase = (BootPhase)data[1],
            ActiveSlot = data[2],
            PendingSlot = data[3],
            AttemptsLeft = data[4],
            Sequence = BitConverter.ToUInt32(data, 5),
            ActiveCrc32 = BitConverter.ToUInt32(data, 9),
            PendingCrc32 = BitConverter.ToUInt32(data, 13),
            Slots = []
        };
    }

    /// <summary>
    /// 查询设备防降级下限。旧Bootloader不支持该命令时返回null，不影响原有升级。
    /// </summary>
    public async Task<FirmwareSecurityStatus?> GetFirmwareSecurityStatus()
    {
        Response? response = await PacketRequest(OPCODE.SECURITY_STATUS, null,
                                                 TimeSpan.FromSeconds(2));
        byte[]? data = response?.Param;
        if (response?.Errcode != ERRCODE.OK || data == null || data.Length != 9)
            return null;

        return new FirmwareSecurityStatus
        {
            MinimumVersion = BitConverter.ToUInt32(data, 0),
            PendingVersion = BitConverter.ToUInt32(data, 4),
            DowngradeAuthorized = data[8] != 0
        };
    }

    /// <summary>
    /// 获取升级选槽所需的最小状态。旧Bootloader不认识快速命令时自动使用原有
    /// 完整状态命令，因此新上位机仍可维护已经部署的旧版本。
    /// </summary>
    async Task<BootStatus?> GetUpgradeBootStatus()
    {
        BootStatus? status = await GetBootStatusFast();
        if (status != null)
            return status;

        OnEvent?.Invoke("快速状态查询不可用，回退到完整A/B检查");
        return await GetBootStatus();
    }

    /// <summary>
    /// 构造断点扫描请求。CRC列表只覆盖应用数据区，每个元素对应一个4 KiB块，
    /// 最后一块按实际数据长度计算，不包含扇区尾部的擦除填充。
    /// </summary>
    static byte[] BuildResumeScanRequest(byte slot, byte[] xbin,
                                         MagicHeader header)
    {
        int dataOffset = checked((int)header.DataOffset);
        int dataLength = checked((int)header.DataLength);
        int blockCount = (dataLength + RESUME_BLOCK_SIZE - 1) /
                         RESUME_BLOCK_SIZE;
        byte[] param = new byte[7 + blockCount * sizeof(uint)];

        param[0] = slot;
        BitConverter.GetBytes(header.DataLength).CopyTo(param, 1);
        BitConverter.GetBytes((ushort)blockCount).CopyTo(param, 5);
        for (int block = 0; block < blockCount; ++block)
        {
            int offset = block * RESUME_BLOCK_SIZE;
            int size = Math.Min(RESUME_BLOCK_SIZE, dataLength - offset);
            uint crc = CRC.CRC32(xbin.AsSpan(dataOffset + offset, size));
            BitConverter.GetBytes(crc).CopyTo(param, 7 + block * sizeof(uint));
        }
        return param;
    }

    /// <summary>
    /// 让Bootloader使非活动槽Header失效，并返回与当前XBIN连续匹配的数据末端。
    /// 旧Bootloader明确返回OPCODE时标记为不支持；超时或其他错误不会被误当成
    /// 旧版本，因为此时继续擦除可能掩盖真实链路故障。
    /// </summary>
    async Task<ResumeScanResult> ScanResumeOffset(byte slot, byte[] xbin,
                                                  MagicHeader header)
    {
        byte[] param = BuildResumeScanRequest(slot, xbin, header);
        Response? response = await PacketRequest(OPCODE.SLOT_RESUME_SCAN, param,
                                                 TimeSpan.FromSeconds(5));
        if (response == null)
            return new ResumeScanResult(true, false, 0);
        if (response.Errcode == ERRCODE.OPCODE)
            return new ResumeScanResult(false, true, 0);
        if (response.Errcode != ERRCODE.OK || response.Param?.Length != 4)
            return new ResumeScanResult(true, false, 0);

        uint offset = BitConverter.ToUInt32(response.Param, 0);
        uint dataEnd = header.DataOffset + header.DataLength;
        bool alignedBlock = offset >= header.DataOffset && offset < dataEnd &&
                            (offset - header.DataOffset) % RESUME_BLOCK_SIZE == 0;
        if (offset != dataEnd && !alignedBlock)
            return new ResumeScanResult(true, false, 0);

        return new ResumeScanResult(true, true, offset);
    }

    async Task<bool> W25Erase(byte slot, uint address, uint size)
    {
        byte[] param = new byte[9];
        param[0] = slot;
        BitConverter.GetBytes(address).CopyTo(param, 1);
        BitConverter.GetBytes(size).CopyTo(param, 5);
        Response? response = await PacketRequest(OPCODE.W25_ERASE, param,
                                                 TimeSpan.FromSeconds(20));
        return response?.Errcode == ERRCODE.OK;
    }

    async Task<bool> W25Program(byte slot, uint address, byte[] data, int mtu,
                                int progressBase, int progressTotal)
    {
        int offset = 0;
        int chunkMax = Math.Min(W25_PROGRAM_DATA_MAX,
                                mtu - W25_PROGRAM_PARAM_HEADER_SIZE);

        if (chunkMax <= 0)
            return false;

        while (offset < data.Length)
        {
            int chunk = Math.Min(chunkMax, data.Length - offset);
            byte[] param = new byte[W25_PROGRAM_PARAM_HEADER_SIZE + chunk];
            param[0] = slot;
            BitConverter.GetBytes(address + (uint)offset).CopyTo(param, 1);
            BitConverter.GetBytes((ushort)chunk).CopyTo(param, 5);
            Array.Copy(data, offset, param, W25_PROGRAM_PARAM_HEADER_SIZE, chunk);

            /*
             * 中间块收到的是“CRC和参数已接受”ACK：MCU随后写当前块，PC同时
             * 发送下一块。最后一块改用同步命令，等所有实际写入完成后才返回。
             */
            OPCODE opcode = offset + chunk < data.Length
                            ? OPCODE.W25_PROGRAM_ASYNC
                            : OPCODE.W25_PROGRAM;
            Response? response = await PacketRequest(opcode, param,
                                                     TimeSpan.FromSeconds(5));
            if (response?.Errcode != ERRCODE.OK)
                return false;

            offset += chunk;
            OnProgress?.Invoke((double)(progressBase + offset) /
                               progressTotal * 100.0);
        }
        return true;
    }

    async Task<bool> SnapshotCurrentApplication(byte slot)
    {
        OnEvent?.Invoke($"首次使用，正在把当前内部固件备份到 Slot {(char)('A' + slot)}");
        Response? response = await PacketRequest(OPCODE.SLOT_SNAPSHOT, [slot],
                                                 TimeSpan.FromSeconds(30));
        return response?.Errcode == ERRCODE.OK;
    }

    async Task<bool> ActivateSlot(byte slot, uint expectedDataCrc)
    {
        byte[] param = new byte[5];
        param[0] = slot;
        BitConverter.GetBytes(expectedDataCrc).CopyTo(param, 1);

        Response? response = await PacketRequest(OPCODE.SLOT_ACTIVATE, param,
                                                 TimeSpan.FromSeconds(15));

        /* v1.3及更早版本只接受1字节槽号，保留一次兼容回退。 */
        if (response?.Errcode == ERRCODE.PARAM)
        {
            OnEvent?.Invoke("旧版Bootloader不支持期望CRC参数，使用兼容激活格式");
            response = await PacketRequest(OPCODE.SLOT_ACTIVATE, [slot],
                                           TimeSpan.FromSeconds(15));
        }
        if (response?.Errcode == ERRCODE.DENIED)
            OnEvent?.Invoke("Bootloader拒绝安装：固件安全版本低于设备下限，需使用新签名回退包或KEY2授权");
        return response?.Errcode == ERRCODE.OK;
    }

    /// <summary>
    /// 把XBIN写入非活动槽，完成签名校验后提交为InstallPending。
    ///
    /// 默认先使用4 KiB分块CRC查找已经落盘的连续前缀，只擦写首个不一致块
    /// 之后的区域。旧Bootloader不支持扫描Opcode时自动执行原有全量流程。
    /// 无论采用哪条路径，Header都最后写入，提交前不会改写内部应用或活动槽。
    /// </summary>
    /// <param name="forceFull">true时跳过断点扫描并完整擦写目标槽。</param>
    public async Task<bool> UpgradeExternalAB(byte[] xbin, int mtu,
                                              MagicHeader header,
                                              bool forceFull = false)
    {
        Stopwatch totalTimer = Stopwatch.StartNew();
        Stopwatch stepTimer = Stopwatch.StartNew();
        BootStatus? status = await GetUpgradeBootStatus();
        OnEvent?.Invoke($"读取A/B状态耗时: {stepTimer.Elapsed.TotalSeconds:F2} 秒");
        if (status == null)
        {
            OnEvent?.Invoke("无法读取A/B状态");
            return false;
        }

        if (!status.StateFound)
        {
            if (!await SnapshotCurrentApplication(0))
            {
                OnEvent?.Invoke("当前固件快照失败");
                return false;
            }
            status = await GetUpgradeBootStatus();
        }

        if (status == null || status.ActiveSlot >= 2)
        {
            OnEvent?.Invoke("没有可用的已确认活动槽");
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
            if (!scan.Supported)
            {
                OnEvent?.Invoke("Bootloader不支持断点扫描，回退到兼容的完整升级");
            }
            else if (!scan.Success)
            {
                OnEvent?.Invoke("断点扫描失败，未擦写活动固件");
                return false;
            }
            else
            {
                resumable = true;
                resumeOffset = scan.Offset;
                OnEvent?.Invoke($"断点扫描耗时: {stepTimer.Elapsed.TotalSeconds:F2} 秒");
            }
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

            /* 兼容旧Bootloader：保持原有布局传输，Header仍在最后提交。 */
            byte[] body = xbin[256..];
            if (!await W25Program(target, 256, body, mtu, 0, xbin.Length) ||
                !await W25Program(target, 0, xbin[..256], mtu,
                                  body.Length, xbin.Length))
                return false;
        }
        else
        {
            uint dataEnd = header.DataOffset + header.DataLength;
            int progressTotal = checked((int)header.DataLength + 256);
            bool headerSectorErased = false;

            if (resumeOffset < dataEnd)
            {
                /*
                 * 从前一个32 KiB边界重写少量已匹配数据，使W25Q驱动可以优先
                 * 使用Block Erase，避免从非对齐地址连续执行大量4 KiB擦除。
                 */
                uint eraseStart = resumeOffset & ~(RESUME_ERASE_ALIGNMENT - 1u);
                uint writeStart = Math.Max(eraseStart, header.DataOffset);
                uint eraseEnd = (dataEnd + RESUME_BLOCK_SIZE - 1u) &
                                ~(uint)(RESUME_BLOCK_SIZE - 1);
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
                !await W25Erase(target, 0, RESUME_BLOCK_SIZE))
                return false;
            if (!await W25Program(target, 0, xbin[..256], mtu,
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
        Response? response = await PacketRequest(OPCODE.RESET, null, TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS));
        if (response != null && response.Errcode == ERRCODE.OK)
        {
            return true;
        }
        return false;
    }

    /// <summary>请求Bootloader通过统一A/B状态机准备并启动应用。</summary>
    public async Task<bool> Boot()
    {
        Response? response = await PacketRequest(OPCODE.BOOT, null, TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS));
        if (response != null && response.Errcode == ERRCODE.OK)
        {
            return true;
        }
        return false;
    }
}
