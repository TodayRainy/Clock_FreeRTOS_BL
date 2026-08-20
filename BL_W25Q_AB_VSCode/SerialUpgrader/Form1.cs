using System.IO.Ports;

namespace SerialUpgrader;

/// <summary>串口 A/B 固件升级主窗口。</summary>
public partial class Form1 : Form
{
    public Form1()
    {
        InitializeComponent();
    }

    private void Form1_Load(object sender, EventArgs e)
    {
        comboBoxSerialPort.Items.Clear();
        string[] ports = SerialPort.GetPortNames();
        if (ports.Length > 0)
        {
            comboBoxSerialPort.Items.AddRange(ports);
            comboBoxSerialPort.SelectedIndex = 0;
        }
    }

    private void buttonSelectFirmware_Click(object sender, EventArgs e)
    {
        using OpenFileDialog dialog = new()
        {
            Filter = "XBIN firmware (*.xbin)|*.xbin"
        };

        if (dialog.ShowDialog() == DialogResult.OK)
            textBoxFirmware.Text = dialog.FileName;
    }

    private async void buttonUpgrade_Click(object sender, EventArgs e)
    {
        buttonUpgrade.Enabled = false;
        try
        {
            await UpgradeSelectedFirmware();
        }
        catch (Exception ex)
        {
            AppendLog($"升级异常: {ex.Message}");
        }
        finally
        {
            buttonUpgrade.Enabled = true;
        }
    }

    /// <summary>校验用户选择并执行完整的 Formal A/B 升级流程。</summary>
    private async Task UpgradeSelectedFirmware()
    {
        string firmwarePath = textBoxFirmware.Text;
        if (string.IsNullOrEmpty(firmwarePath) || !File.Exists(firmwarePath))
        {
            MessageBox.Show("先选择固件文件");
            return;
        }

        string? port = comboBoxSerialPort.SelectedItem as string;
        if (string.IsNullOrEmpty(port))
        {
            MessageBox.Show("先选择串口");
            return;
        }

        textBoxLog.Clear();
        progressBar.Value = 0;

        byte[] xbin = await File.ReadAllBytesAsync(firmwarePath);
        AppendLog($"固件文件: {Path.GetFileName(firmwarePath)}");

        MagicHeader? header = MagicHeader.Parse(xbin);
        if (header == null)
        {
            AppendLog("无效的固件文件");
            return;
        }
        AppendFirmwareInfo(header);

        using Transfer transfer = new(port);
        transfer.OnProgress += percent =>
        {
            if (!IsDisposed)
            {
                BeginInvoke(() =>
                    progressBar.Value = Math.Clamp((int)percent, 0, 100));
            }
        };
        transfer.OnEvent += message =>
        {
            if (!IsDisposed && !string.IsNullOrEmpty(message))
                BeginInvoke(() => AppendLog(message));
        };

        if (!transfer.Open())
        {
            AppendLog("无法打开串口");
            return;
        }

        string? version = await transfer.GetBootloaderVersion();
        if (version == null)
        {
            AppendLog("无法连接到引导程序");
            return;
        }
        AppendLog($"Bootloader Version: {version}");

        int? mtu = await transfer.GetBootloaderMTU();
        if (mtu == null)
        {
            AppendLog("无法获取MTU");
            return;
        }
        AppendLog($"MTU: {mtu}");

        if (!await transfer.SetBaudRate(Transfer.UpgradeBaudRate))
        {
            AppendLog("高速链路协商失败，升级尚未开始");
            return;
        }

        AppendLog("开始写入外部非活动固件槽");
        bool success = await transfer.UpgradeExternalAB(
            xbin, mtu.Value, header, checkBoxForceFull.Checked);
        if (!success)
        {
            AppendLog("A/B升级提交失败；原活动固件保持不变");
            return;
        }

        AppendLog("外部固件校验并提交成功");
        AppendLog("设备将自动安装并试运行；5秒稳定运行后确认，否则三次失败后回滚");
    }

    /// <summary>显示已经通过本地结构和 Header CRC 检查的 Formal 元数据。</summary>
    private void AppendFirmwareInfo(MagicHeader header)
    {
        AppendLog($"格式: XBIN Formal revision {header.FormatRevision}");
        AppendLog($"固件版本: {header.Version}");
        AppendLog($"安全版本: {header.SecurityVersion}");
        AppendLog($"发布序号: {header.ReleaseSequence}");
        AppendLog($"固件类型: {header.DataType}");
        AppendLog($"固件偏移: {header.DataOffset}");
        AppendLog($"固件地址: 0x{header.DataAddress:X8}");
        AppendLog($"固件校验: 0x{header.DataCrc32:X8}");
        AppendLog($"签名 Key ID: 0x{header.SigningKeyId:X8}");
        AppendLog($"SHA-256: {Convert.ToHexString(header.ImageSha256)}");
        AppendLog($"加密: AES-256-GCM (key 0x{header.EncryptionKeyId:X8})");
        AppendLog($"固件大小: {header.DataLength} ({header.DataLength / 1024.0:F2} KB)");
    }

    private void AppendLog(string message)
    {
        textBoxLog.AppendText(message + Environment.NewLine);
    }
}
