using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace SerialUpgrader;

public enum MagicHeaderDataType : uint
{
    Application = 1,
}

/// <summary>
/// XBIN v2的固定256字节固件描述头。上位机负责检查结构、CRC32和SHA-256，
/// Bootloader使用设备内置公钥完成最终ECDSA签名验证。
/// </summary>
public class MagicHeader
{
    private const uint CurrentFormatVersion = 2;
    private const uint HeaderSize = 256;

    public uint FormatVersion { get; private set; }
    public uint KeyId { get; private set; }
    public uint SecurityVersion { get; private set; }
    public MagicHeaderDataType DataType { get; private set; }
    public uint DataOffset { get; private set; }
    public uint DataAddress { get; private set; }
    public uint DataLength { get; private set; }
    public uint DataCrc32 { get; private set; }
    public byte[] ImageSha256 { get; private set; } = [];
    public string Version { get; private set; } = string.Empty;
    public uint ThisAddress { get; private set; }

    /// <summary>
    /// 解析并检查待发送的XBIN。这里的SHA-256用于尽早发现本地文件损坏；公钥
    /// 授权判断始终由Bootloader执行，不能由可替换的PC上位机代替。
    /// </summary>
    public static MagicHeader? Parse(byte[] xbin)
    {
        if (xbin.Length < HeaderSize)
        {
            Trace.WriteLine("数据长度不足256字节，无法解析头部信息");
            return null;
        }

        uint magic = BitConverter.ToUInt32(xbin, 0);
        uint formatVersion = BitConverter.ToUInt32(xbin, 4);
        uint headerSize = BitConverter.ToUInt32(xbin, 8);
        uint hashAlgorithm = BitConverter.ToUInt32(xbin, 12);
        uint signatureAlgorithm = BitConverter.ToUInt32(xbin, 16);
        uint keyId = BitConverter.ToUInt32(xbin, 20);
        uint securityVersion = BitConverter.ToUInt32(xbin, 24);
        uint dataType = BitConverter.ToUInt32(xbin, 32);
        uint dataOffset = BitConverter.ToUInt32(xbin, 36);
        uint dataAddress = BitConverter.ToUInt32(xbin, 40);
        uint dataLength = BitConverter.ToUInt32(xbin, 44);
        uint dataCrc32 = BitConverter.ToUInt32(xbin, 48);
        byte[] imageSha256 = xbin[52..84];
        string version = Encoding.ASCII.GetString(xbin, 96, 64).TrimEnd('\0');
        ReadOnlySpan<byte> signature = xbin.AsSpan(160, 64);
        uint thisAddress = BitConverter.ToUInt32(xbin, 248);
        uint thisCrc32 = BitConverter.ToUInt32(xbin, 252);

        if (magic != 0x4D414749)
        {
            Trace.WriteLine($"无效的头部数据，magic: 0x{magic:X8}");
            return null;
        }

        uint computedCrc32 = CRC.CRC32(xbin[..252]);
        if (thisCrc32 != computedCrc32)
        {
            Trace.WriteLine($"头部数据CRC32校验失败，expected: 0x{thisCrc32:X8}, got: 0x{computedCrc32:X8}");
            return null;
        }

        if (formatVersion != CurrentFormatVersion || headerSize != HeaderSize ||
            hashAlgorithm != 1 || signatureAlgorithm != 1 ||
            signature.IndexOfAnyExcept((byte)0) < 0 ||
            dataType != (uint)MagicHeaderDataType.Application ||
            dataOffset != 4096 || dataAddress != 0x08010000 ||
            dataLength == 0 || dataLength > 0x70000 ||
            thisAddress != 0x0800C000 ||
            dataOffset + dataLength != xbin.Length)
        {
            Trace.WriteLine("签名格式、地址或长度不符合XBIN v2契约");
            return null;
        }

        uint computedDataCrc32 = CRC.CRC32(xbin[(int)dataOffset..]);
        if (computedDataCrc32 != dataCrc32)
        {
            Trace.WriteLine($"固件数据CRC32校验失败，expected: 0x{dataCrc32:X8}, got: 0x{computedDataCrc32:X8}");
            return null;
        }

        byte[] computedSha256 = SHA256.HashData(xbin.AsSpan((int)dataOffset));
        if (!computedSha256.SequenceEqual(imageSha256))
        {
            Trace.WriteLine("固件数据SHA-256校验失败");
            return null;
        }

        return new MagicHeader
        {
            FormatVersion = formatVersion,
            KeyId = keyId,
            SecurityVersion = securityVersion,
            DataType = (MagicHeaderDataType)dataType,
            DataOffset = dataOffset,
            DataAddress = dataAddress,
            DataLength = dataLength,
            DataCrc32 = dataCrc32,
            ImageSha256 = imageSha256,
            Version = version,
            ThisAddress = thisAddress,
        };
    }
}
