using System.Diagnostics;
using System.Text;

namespace SerialUpgrader;

public enum MagicHeaderDataType : uint
{
    Application = 0,
}

public class MagicHeader
{
    /*
     * Magic Header C语言定义
     * 
     * typedef struct
     * {
     *     uint32_t magic;         // 魔数，用于标识这是一个有效的魔术头
     *     uint32_t bitmask;       // 位掩码，用于标识哪些字段有效
     *     uint32_t reserved1[6];  // 保留字段，供将来扩展使用
     * 
     *     uint32_t data_type;     // 类型，根据type类型选择固件下载位置
     *     uint32_t data_offset;   // 固件文件相对于magic header的偏移
     *     uint32_t data_address;  // 固件写入的实际地址
     *     uint32_t data_length;   // 固件长度
     *     uint32_t data_crc32;    // 固件的CRC32校验值
     *     uint32_t reserved2[11]; // 保留字段，供将来扩展使用
     * 
     *     char version[128];      // 固件版本字符串
     * 
     *     uint32_t reserved3[6];  // 保留字段，供将来扩展使用
     *     uint32_t this_address;  // 该结构体在存储介质中的实际地址
     *     uint32_t this_crc32;    // 该结构体本身的CRC32校验值
     * } magic_header_t;
     */

    public MagicHeaderDataType DataType { get; private set; }
    public uint DataOffset { get; private set; }
    public uint DataAddress { get; private set; }
    public uint DataLength { get; private set; }
    public uint DataCrc32 { get; private set; }
    public string Version { get; private set; } = string.Empty;
    public uint ThisAddress { get; private set; }

    public static MagicHeader? Parse(byte[] xbin)
    {
        if (xbin.Length < 256)
        {
            Trace.WriteLine("数据长度不足256字节，无法解析头部信息");
            return null;
        }

        uint magic = BitConverter.ToUInt32(xbin, 0);
        if (magic != 0x4D414749) // "MAGI"
        {
            Trace.WriteLine($"无效的头部数据，magic: 0x{magic:X8}");
            return null;
        }

        //uint bitmask = BitConverter.ToUInt32(xbin, 4);

        uint dataType = BitConverter.ToUInt32(xbin, 32);

        uint dataOffset = BitConverter.ToUInt32(xbin, 36);

        uint dataAddress = BitConverter.ToUInt32(xbin, 40);

        uint dataLength = BitConverter.ToUInt32(xbin, 44);

        uint dataCrc32 = BitConverter.ToUInt32(xbin, 48);

        string version = Encoding.ASCII.GetString(xbin, 96, 128).TrimEnd('\0');

        uint thisAddress = BitConverter.ToUInt32(xbin, 248);

        uint thisCrc32 = BitConverter.ToUInt32(xbin, 252);

        uint computedCrc32 = CRC.CRC32(xbin[..252]);

        if (thisCrc32 != computedCrc32)
        {
            Trace.WriteLine($"头部数据CRC32校验失败，expected: 0x{thisCrc32:X8}, got: 0x{computedCrc32:X8}");
            return null;
        }

        MagicHeader header = new();
        header.DataType = (MagicHeaderDataType)dataType;
        header.DataOffset = dataOffset;
        header.DataAddress = dataAddress;
        header.DataLength = dataLength;
        header.DataCrc32 = dataCrc32;
        header.Version = version;
        header.ThisAddress = thisAddress;

        return header;
    }
}
