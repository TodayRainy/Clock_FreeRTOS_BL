using System.Diagnostics;
using System.Text;

namespace SerialUpgrader;

public enum MagicHeaderDataType : uint
{
    Application = 1,
}

/// <summary>
/// XBIN Formal revision 1的固定320字节Header。Formal只允许ECDSA-P256签名和
/// AES-256-GCM加密，上位机不持有解密密钥，最终安全校验始终由Bootloader执行。
/// </summary>
public sealed class MagicHeader
{
    private const uint FormalMagic = 0x4D524F46; // "FORM"
    private const uint FormalRevision = 1;
    private const int HeaderSize = 320;
    private const int HeaderCrcOffset = 316;
    private const int HeaderAreaSize = 4096;

    public uint FormatRevision { get; private set; }
    public uint SigningKeyId { get; private set; }
    public uint EncryptionKeyId { get; private set; }
    public uint SecurityVersion { get; private set; }
    public uint ReleaseSequence { get; private set; }
    public MagicHeaderDataType DataType { get; private set; }
    public uint DataOffset { get; private set; }
    public uint DataAddress { get; private set; }
    public uint DataLength { get; private set; }
    public uint DataCrc32 { get; private set; }
    public byte[] ImageSha256 { get; private set; } = [];
    public string Version { get; private set; } = string.Empty;
    public int HeaderCommitSize => HeaderSize;

    /// <summary>
    /// 解析并检查本地XBIN的Formal布局和Header CRC。由于正文是密文，上位机
    /// 不尝试验证明文摘要；签名、GCM Tag及明文CRC/SHA由Bootloader独立完成。
    /// </summary>
    public static MagicHeader? Parse(byte[] xbin)
    {
        if (xbin.Length <= HeaderAreaSize)
        {
            Trace.WriteLine("文件不足4096字节Formal Header区或没有固件正文");
            return null;
        }

        uint magic = BitConverter.ToUInt32(xbin, 0);
        uint revision = BitConverter.ToUInt32(xbin, 4);
        uint headerSize = BitConverter.ToUInt32(xbin, 8);
        uint dataType = BitConverter.ToUInt32(xbin, 12);
        uint dataOffset = BitConverter.ToUInt32(xbin, 16);
        uint dataAddress = BitConverter.ToUInt32(xbin, 20);
        uint dataLength = BitConverter.ToUInt32(xbin, 24);
        uint dataCrc32 = BitConverter.ToUInt32(xbin, 28);
        uint securityVersion = BitConverter.ToUInt32(xbin, 32);
        uint releaseSequence = BitConverter.ToUInt32(xbin, 36);
        uint hashAlgorithm = BitConverter.ToUInt32(xbin, 40);
        uint signatureAlgorithm = BitConverter.ToUInt32(xbin, 44);
        uint encryptionAlgorithm = BitConverter.ToUInt32(xbin, 48);
        uint signingKeyId = BitConverter.ToUInt32(xbin, 52);
        uint encryptionKeyId = BitConverter.ToUInt32(xbin, 56);
        uint reserved0 = BitConverter.ToUInt32(xbin, 60);
        byte[] imageSha256 = xbin[64..96];
        string version = Encoding.ASCII.GetString(xbin, 124, 64).TrimEnd('\0');
        ReadOnlySpan<byte> signature = xbin.AsSpan(188, 64);
        uint expectedHeaderCrc = BitConverter.ToUInt32(xbin, HeaderCrcOffset);
        uint actualHeaderCrc = CRC.CRC32(xbin[..HeaderCrcOffset]);

        if (magic != FormalMagic || revision != FormalRevision ||
            headerSize != HeaderSize)
        {
            Trace.WriteLine("固件不是XBIN Formal revision 1");
            return null;
        }
        if (expectedHeaderCrc != actualHeaderCrc)
        {
            Trace.WriteLine($"Formal Header CRC32失败，expected=0x{expectedHeaderCrc:X8}, got=0x{actualHeaderCrc:X8}");
            return null;
        }
        if (dataType != (uint)MagicHeaderDataType.Application ||
            dataOffset != HeaderAreaSize || dataAddress != 0x08010000 ||
            dataLength == 0 || dataLength > 0x70000 ||
            dataOffset + dataLength != xbin.Length ||
            hashAlgorithm != 1 || signatureAlgorithm != 1 ||
            encryptionAlgorithm != 1 || signingKeyId == 0 ||
            encryptionKeyId == 0 || reserved0 != 0 ||
            string.IsNullOrEmpty(version) ||
            signature.IndexOfAnyExcept((byte)0) < 0 ||
            xbin.AsSpan(252, 64).IndexOfAnyExcept((byte)0) >= 0 ||
            xbin.AsSpan(HeaderSize, HeaderAreaSize - HeaderSize)
                .IndexOfAnyExcept((byte)0) >= 0)
        {
            Trace.WriteLine("Formal算法、地址、长度或保留区不符合契约");
            return null;
        }

        return new MagicHeader
        {
            FormatRevision = revision,
            SigningKeyId = signingKeyId,
            EncryptionKeyId = encryptionKeyId,
            SecurityVersion = securityVersion,
            ReleaseSequence = releaseSequence,
            DataType = (MagicHeaderDataType)dataType,
            DataOffset = dataOffset,
            DataAddress = dataAddress,
            DataLength = dataLength,
            DataCrc32 = dataCrc32,
            ImageSha256 = imageSha256,
            Version = version,
        };
    }
}
