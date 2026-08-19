using System.Diagnostics;

namespace SerialUpgrader
{
    internal static class Program
    {
        /// <summary>
        ///  The main entry point for the application.
        /// </summary>
        [STAThread]
        static int Main(string[] args)
        {
            if ((args.Length == 3 || args.Length == 4) &&
                (args[0] == "--upgrade" || args[0] == "--upgrade-full"))
            {
                bool forceFull = args[0] == "--upgrade-full";
                int baudRate = Transfer.UpgradeBaudRate;
                if (args.Length == 4 && !int.TryParse(args[3], out baudRate))
                {
                    Console.WriteLine($"无效的波特率: {args[3]}");
                    return 1;
                }
                return RunCommandLineUpgrade(args[1], args[2], baudRate,
                                             forceFull)
                       .GetAwaiter().GetResult();
            }
            if (args.Length == 2 && args[0] == "--status")
                return RunCommandLineStatus(args[1]).GetAwaiter().GetResult();
            if (args.Length == 2 && args[0] == "--maintenance-probe")
                return RunMaintenanceProbe(args[1]).GetAwaiter().GetResult();

            // To customize application configuration such as set high DPI settings or default font,
            // see https://aka.ms/applicationconfiguration.
            ApplicationConfiguration.Initialize();
            Application.Run(new Form1());
            return 0;
        }

        static async Task<int> RunCommandLineUpgrade(string port, string firmware,
                                                     int baudRate,
                                                     bool forceFull)
        {
            if (!File.Exists(firmware))
            {
                Console.WriteLine($"固件文件不存在: {firmware}");
                return 1;
            }

            byte[] xbin = await File.ReadAllBytesAsync(firmware);
            MagicHeader? header = MagicHeader.Parse(xbin);
            if (header == null)
            {
                Console.WriteLine("XBIN校验失败");
                return 1;
            }

            Console.WriteLine($"XBIN v{header.FormatVersion}, Key ID 0x{header.KeyId:X8}");
            Console.WriteLine($"安全版本: {header.SecurityVersion}");
            Console.WriteLine($"SHA-256 {Convert.ToHexString(header.ImageSha256)}");

            using Transfer transfer = new(port);
            transfer.OnEvent += Console.WriteLine;
            transfer.OnProgress += value => Console.WriteLine($"进度: {value:F1}%");

            Stopwatch totalTimer = Stopwatch.StartNew();
            if (!transfer.Open())
                return 1;

            string? version = await transfer.GetBootloaderVersion();
            int? mtu = await transfer.GetBootloaderMTU();
            if (version == null || mtu == null)
            {
                Console.WriteLine("无法连接Bootloader");
                return 1;
            }
            Console.WriteLine($"Bootloader {version}, MTU {mtu}");

            if (!await transfer.SetBaudRate(baudRate))
            {
                Console.WriteLine("高速链路协商失败，升级尚未开始");
                return 1;
            }

            bool ok = await transfer.UpgradeExternalAB(xbin, mtu.Value, header,
                                                       forceFull);
            Console.WriteLine(ok ? "升级已提交" : "升级失败，活动固件未改变");
            Console.WriteLine($"含握手总耗时: {totalTimer.Elapsed.TotalSeconds:F2} 秒");
            return ok ? 0 : 1;
        }

        static async Task<int> RunCommandLineStatus(string port)
        {
            using Transfer transfer = new(port);
            if (!transfer.Open())
                return 1;
            string? version = await transfer.GetBootloaderVersion();
            BootStatus? status = version == null ? null : await transfer.GetBootStatus();
            FirmwareSecurityStatus? security = version == null ? null :
                                                       await transfer.GetFirmwareSecurityStatus();
            if (version == null || status == null)
            {
                Console.WriteLine("无法连接Bootloader或读取A/B状态");
                return 1;
            }

            Console.WriteLine($"Bootloader {version}");
            Console.WriteLine($"BCR有效: {status.StateFound}, phase={status.Phase}, " +
                              $"active={SlotName(status.ActiveSlot)}, pending={SlotName(status.PendingSlot)}, " +
                              $"attempts={status.AttemptsLeft}, seq={status.Sequence}");
            Console.WriteLine($"active CRC=0x{status.ActiveCrc32:X8}, pending CRC=0x{status.PendingCrc32:X8}");
            if (security != null)
            {
                Console.WriteLine($"安全版本下限={security.MinimumVersion}, " +
                                  $"pending={security.PendingVersion}, " +
                                  $"KEY2降级授权={security.DowngradeAuthorized}");
            }
            for (int i = 0; i < status.Slots.Length; ++i)
            {
                FirmwareSlotInfo slot = status.Slots[i];
                Console.WriteLine($"Slot {(char)('A' + i)}: header={slot.HeaderValid}, " +
                                  $"full={slot.FullValid}, size={slot.DataLength}, " +
                                  $"CRC=0x{slot.DataCrc32:X8}, version={slot.Version}");
            }

            /* 查询命令会把Bootloader留在接收循环，查询完主动启动应用。 */
            if (!await transfer.Boot())
            {
                Console.WriteLine("状态已读取，但未能发送启动应用命令");
                return 1;
            }
            return 0;
        }

        /// <summary>
        /// 以只读W25Q命令验证DEV/PROD维护权限，不修改Flash、EEPROM或BCR。
        /// </summary>
        static async Task<int> RunMaintenanceProbe(string port)
        {
            using Transfer transfer = new(port);
            if (!transfer.Open())
                return 1;

            string? version = await transfer.GetBootloaderVersion();
            bool? allowed = version == null ? null : await transfer.IsMaintenanceReadAllowed();
            if (version == null || allowed == null)
            {
                Console.WriteLine("无法连接Bootloader或维护权限响应异常");
                return 1;
            }

            Console.WriteLine($"Bootloader {version}");
            Console.WriteLine(allowed.Value
                ? "维护接口: ALLOWED"
                : "维护接口: DENIED");

            /* 探测结束后恢复正常启动，不让设备停留在串口接收循环。 */
            if (!await transfer.Boot())
            {
                Console.WriteLine("权限已读取，但未能发送启动应用命令");
                return 1;
            }
            return 0;
        }

        static string SlotName(byte slot)
        {
            return slot < 2 ? ((char)('A' + slot)).ToString() : "none";
        }
    }
}
