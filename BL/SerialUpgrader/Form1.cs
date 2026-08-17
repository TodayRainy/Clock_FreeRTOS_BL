using System.IO.Ports;

namespace SerialUpgrader
{
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
            OpenFileDialog ofd = new()
            {
                Filter = "firmware|*.xbin"
            };
            if (ofd.ShowDialog() == DialogResult.OK)
            {
                textBoxFirmware.Text = ofd.FileName;
            }
        }

        private async void buttonUpgrade_Click(object sender, EventArgs e)
        {
            string firmware = textBoxFirmware.Text;
            if (string.IsNullOrEmpty(firmware) || !File.Exists(firmware))
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

            FileInfo xbininfo = new(firmware);
            byte[] xbin = File.ReadAllBytes(firmware);
            if (xbin.Length != xbininfo.Length)
            {
                textBoxLog.AppendText($"文件大小与文件信息大小不匹配{xbin.Length}!={xbininfo.Length}\r\n");
                return;
            }

            textBoxLog.AppendText($"固件文件: {xbininfo.Name}\r\n");

            MagicHeader? header = MagicHeader.Parse(xbin);
            if (header == null)
            {
                textBoxLog.AppendText("无效的固件文件\r\n");
                return;
            }

            byte[] headerbin = xbin[..(int)header.DataOffset];
            byte[] databin = xbin[(int)header.DataOffset..];

            if (databin.Length != header.DataLength)
            {
                textBoxLog.AppendText($"固件数据长度与头部信息不匹配 {databin.Length}!={header.DataLength}\r\n");
                return;
            }

            textBoxLog.AppendText($"头部地址: 0x{header.ThisAddress:X8}\r\n");
            textBoxLog.AppendText($"固件版本: {header.Version}\r\n");
            textBoxLog.AppendText($"固件类型: {header.DataType}\r\n");
            textBoxLog.AppendText($"固件偏移: {header.DataOffset}\r\n");
            textBoxLog.AppendText($"固件地址: 0x{header.DataAddress:X8}\r\n");
            textBoxLog.AppendText($"固件校验: 0x{header.DataCrc32:X8}\r\n");
            textBoxLog.AppendText($"固件大小: {header.DataLength} ({header.DataLength / 1024.0:F2} KB)\r\n");

            Transfer transfer = new(port);
            transfer.OnPorgress += (percent) =>
            {
                if (!textBoxLog.IsDisposed)
                {
                    BeginInvoke(() => progressBar.Value = (int)percent);
                }
            };
            transfer.OnEvent += (message) =>
            {
                if (!textBoxLog.IsDisposed)
                {
                    BeginInvoke(() =>
                    {
                        if (!string.IsNullOrEmpty(message))
                            textBoxLog.AppendText(message + "\r\n");
                    });
                }
            };

            string? version = await transfer.GetBootloaderVersion();
            if (version == null)
            {
                textBoxLog.AppendText("无法连接到引导程序\r\n");
                return;
            }
            textBoxLog.AppendText($"Bootloader Version: {version}\r\n");

            int? mtu = await transfer.GetBootloaderMTU();
            if (mtu == null)
            {
                textBoxLog.AppendText("无法获取MTU\r\n");
                return;
            }
            textBoxLog.AppendText($"MTU: {mtu}\r\n");

            bool opsuccess;
            // 写header
            uint headeraddr = header.ThisAddress;
            uint headercrc = CRC.CRC32(headerbin);
            textBoxLog.AppendText($"写Header {headerbin.Length}@0x{header.ThisAddress:X8} CRC32:0x{headercrc:X8}\r\n");
            textBoxLog.AppendText("开始擦除指定数据块\r\n");
            opsuccess = await transfer.Erase(headeraddr, (uint)headerbin.Length);
            if (!opsuccess)
            {
                textBoxLog.AppendText("擦除失败\r\n");
                return;
            }
            textBoxLog.AppendText("擦除完成\r\n");
            textBoxLog.AppendText("开始写入数据\r\n");
            opsuccess = await transfer.Program(headeraddr, headerbin, (int)mtu);
            if (!opsuccess)
            {
                textBoxLog.AppendText("写入失败\r\n");
                return;
            }
            textBoxLog.AppendText("写入完成\r\n");
            textBoxLog.AppendText("校验写入数据\r\n");
            opsuccess = await transfer.Verify(headeraddr, (uint)headerbin.Length, headercrc);
            if (!opsuccess)
            {
                textBoxLog.AppendText("校验失败\r\n");
                return;
            }
            textBoxLog.AppendText("校验成功\r\n");
            textBoxLog.AppendText("Header写入完毕\r\n");

            uint dataaddr = header.DataAddress;
            uint datacrc = CRC.CRC32(databin);
            textBoxLog.AppendText($"写Data {databin.Length}@0x{header.DataAddress:X8} CRC32:0x{datacrc:X8}\r\n");
            textBoxLog.AppendText("开始擦除指定数据块\r\n");
            opsuccess = await transfer.Erase(dataaddr, (uint)databin.Length);
            if (!opsuccess) 
            {
                textBoxLog.AppendText("擦除失败\r\n");
                return;
            }
            textBoxLog.AppendText("擦除完成\r\n");
            textBoxLog.AppendText("开始写入数据\r\n");
            opsuccess = await transfer.Program(dataaddr, databin, (int)mtu);
            if (!opsuccess)
            {
                textBoxLog.AppendText("写入失败\r\n");
                return;
            }
            textBoxLog.AppendText("写入完成\r\n");
            textBoxLog.AppendText("校验写入数据\r\n");
            opsuccess = await transfer.Verify(dataaddr, (uint)databin.Length, datacrc);
            if (!opsuccess)
            {
                textBoxLog.AppendText("校验失败\r\n");
                return;
            }
            textBoxLog.AppendText("校验成功\r\n");
            textBoxLog.AppendText("Data写入完毕\r\n");

            textBoxLog.AppendText("全部写入完成，准备启动新固件\r\n");
            opsuccess = await transfer.Boot();
            if (!opsuccess)
            {
                textBoxLog.AppendText("启动失败\r\n");
                return;
            }
            textBoxLog.AppendText("启动成功\r\n");
            textBoxLog.AppendText("固件升级完毕，即将重启\r\n");
        }
    }
}
