using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using System.Threading.Channels;
using Timer = System.Timers.Timer;

namespace SerialUpgrader;

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
    
    public Action<double>? OnPorgress;
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
                if (last_rx_time - DateTime.Now > timeout)
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

    public async Task<bool> Program(uint addr, byte[] data, int mtu)
    {
        uint total = (uint)data.Length, written = 0;
        while (data.Length > 0)
        {
            int chunk_size = Math.Min(mtu - PROGRAM_PARAM_HEADER_SIZE, data.Length);
            OnPorgress?.Invoke((double)written / (double)total * 100.0);
            OnEvent?.Invoke($"Program addr:0x{addr:X8} size:{chunk_size} {written}/{total}");
            byte[] param = new byte[PROGRAM_PARAM_HEADER_SIZE + chunk_size];
            BitConverter.GetBytes(addr).CopyTo(param, 0);
            BitConverter.GetBytes((uint)chunk_size).CopyTo(param, 4);

            data.Take(chunk_size).ToArray().CopyTo(param, PROGRAM_PARAM_HEADER_SIZE);
            Response? response = await PacketRequest(OPCODE.PROGRAM, param, TimeSpan.FromSeconds(PROGRAM_TIMEOUT_SECONDS));
            if (response == null || response.Errcode != ERRCODE.OK)
            {
                return false;
            }
            addr += (uint)chunk_size;
            data = data.Skip(chunk_size).ToArray();
            written += (uint)chunk_size;
        }

        return true;
    }

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

    public async Task<bool> Reset()
    {
        Response? response = await PacketRequest(OPCODE.RESET, null, TimeSpan.FromSeconds(DEFAULT_TIMEOUT_SECONDS));
        if (response != null && response.Errcode == ERRCODE.OK)
        {
            return true;
        }
        return false;
    }

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
