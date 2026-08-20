using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace SerialUpgrader;

public enum MagicHeaderDataType : uint
{
    Application = 1,
}

/// <summary>
/// XBIN的固定256字节签名Header。v3在偏移256追加64字节AES-GCM描述；上位机
/// 不持有解密密钥，最终ECDSA、GCM和明文摘要始终由Bootloader验证。
/// </summary>
public class MagicHeader
{
    private const uint CurrentFormatVersion = 2;
    private const uint HeaderSize = 256;
    private const int EncryptionHeaderOffset = 256;
    private const int EncryptionHeaderSize = 64;

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
    public bool IsEncrypted { get; private set; }
    public uint EncryptionKeyId { get; private set; }
    public int HeaderCommitSize => IsEncrypted
                                   ? EncryptionHeaderOffset + EncryptionHeaderSize
                                   : (int)HeaderSize;

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
        bool encrypted = false;
        uint encryptionKeyId = 0;

        if (xbin.Length >= EncryptionHeaderOffset + EncryptionHeaderSize &&
            BitConverter.ToUInt32(xbin, EncryptionHeaderOffset) == 0x33434E45)
        {
            uint encryptionFormat = BitConverter.ToUInt32(xbin, 260);
            uint encryptionSize = BitConverter.ToUInt32(xbin, 264);
            uint encryptionAlgorithm = BitConverter.ToUInt32(xbin, 268);
            encryptionKeyId = BitConverter.ToUInt32(xbin, 272);
            uint encryptionCrc = BitConverter.ToUInt32(xbin, 316);
            uint computedEncryptionCrc = CRC.CRC32(xbin[256..316]);

            if (encryptionFormat != 1 || encryptionSize != EncryptionHeaderSize ||
                encryptionAlgorithm != 1 || encryptionKeyId == 0 ||
                BitConverter.ToUInt32(xbin, 304) != 0 ||
                BitConverter.ToUInt32(xbin, 308) != 0 ||
                BitConverter.ToUInt32(xbin, 312) != 0 ||
                encryptionCrc != computedEncryptionCrc)
            {
                Trace.WriteLine("AES-GCM加密描述无效");
                return null;
            }
            encrypted = true;
        }

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

        if (!encrypted)
        {
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
            IsEncrypted = encrypted,
            EncryptionKeyId = encryptionKeyId,
        };
    }
}
