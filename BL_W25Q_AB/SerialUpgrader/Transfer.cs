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

/// <summary>
/// USART3 Bootloader协议客户端。每个请求均执行一次同步的请求/响应事务；
/// A/B升级由多个已确认的事务组成，最后以SLOT_ACTIVATE提交。
/// </summary>
public class Transfer
{
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
        ERASE = 0x81,
        PROGRAM = 0x82,
        VERIFY = 0x83,
        RESET = 0x21,
        BOOT = 0x22,
        W25_ERASE = 0x42,
        W25_PROGRAM = 0x43,
        SLOT_STATUS = 0x50,
        SLOT_ACTIVATE = 0x52,
        SLOT_SNAPSHOT = 0x53,
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
        Port.DataReceived += OnDataReceived;

        DataChannel = Channel.CreateUnbounded<byte>();
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
        if (timeout == default)
            timeout = TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS);

        try
        {
            byte[] packet = BuildPacket(opcode, param);
            while (DataChannel.Reader.TryRead(out _)) { }
            Port.Open();
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
                if (Port.IsOpen)
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
        int chunkMax = Math.Min(4096, mtu - 7);

        while (offset < data.Length)
        {
            int chunk = Math.Min(chunkMax, data.Length - offset);
            byte[] param = new byte[7 + chunk];
            param[0] = slot;
            BitConverter.GetBytes(address).CopyTo(param, 1);
            BitConverter.GetBytes((ushort)chunk).CopyTo(param, 5);
            Array.Copy(data, offset, param, 7, chunk);

            Response? response = await PacketRequest(OPCODE.W25_PROGRAM, param,
                                                     TimeSpan.FromSeconds(5));
            if (response?.Errcode != ERRCODE.OK)
                return false;

            offset += chunk;
            address += (uint)chunk;
            OnProgress?.Invoke((double)(progressBase + offset) / progressTotal * 100.0);
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

    async Task<bool> ActivateSlot(byte slot)
    {
        Response? response = await PacketRequest(OPCODE.SLOT_ACTIVATE, [slot],
                                                 TimeSpan.FromSeconds(15));
        return response?.Errcode == ERRCODE.OK;
    }

    /// <summary>
    /// 把完整XBIN写入非活动槽，校验后提交为InstallPending。
    /// 提交前不会改写STM32内部应用区或当前活动槽。
    /// </summary>
    public async Task<bool> UpgradeExternalAB(byte[] xbin, int mtu, uint expectedDataCrc)
    {
        BootStatus? status = await GetBootStatus();
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
            status = await GetBootStatus();
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
        OnEvent?.Invoke($"活动槽: {(char)('A' + status.ActiveSlot)}，写入非活动槽: {(char)('A' + target)}");
        OnEvent?.Invoke($"擦除外部槽区: {eraseSize} 字节");
        if (!await W25Erase(target, 0, eraseSize))
            return false;

        /* 首256字节包含有效魔数。先写其余内容，最后提交这256字节。 */
        byte[] body = xbin[256..];
        if (!await W25Program(target, 256, body, mtu, 0, xbin.Length))
            return false;
        if (!await W25Program(target, 0, xbin[..256], mtu, body.Length, xbin.Length))
            return false;

        OnEvent?.Invoke("外部写入完成，正在由Bootloader校验完整固件");
        status = await GetBootStatus();
        FirmwareSlotInfo? written = status?.Slots[target];
        if (written == null || !written.FullValid || written.DataCrc32 != expectedDataCrc)
        {
            OnEvent?.Invoke("外部槽完整校验失败，内部应用区未改动");
            return false;
        }

        OnEvent?.Invoke($"Slot {(char)('A' + target)} 校验通过，提交试运行");
        if (!await ActivateSlot(target))
            return false;

        OnProgress?.Invoke(100);
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
