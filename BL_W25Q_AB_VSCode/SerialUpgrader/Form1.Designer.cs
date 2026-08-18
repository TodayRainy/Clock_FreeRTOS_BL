namespace SerialUpgrader
{
    partial class Form1
    {
        /// <summary>
        ///  Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        ///  Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        ///  Required method for Designer support - do not modify
        ///  the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            label1 = new Label();
            textBoxFirmware = new TextBox();
            buttonSelectFirmware = new Button();
            label2 = new Label();
            comboBoxSerialPort = new ComboBox();
            textBoxLog = new TextBox();
            progressBar = new ProgressBar();
            buttonUpgrade = new Button();
            SuspendLayout();
            // 
            // label1
            // 
            label1.AutoSize = true;
            label1.Location = new Point(28, 22);
            label1.Name = "label1";
            label1.Size = new Size(110, 31);
            label1.TabIndex = 0;
            label1.Text = "固件文件";
            // 
            // textBoxFirmware
            // 
            textBoxFirmware.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            textBoxFirmware.BackColor = SystemColors.Window;
            textBoxFirmware.Location = new Point(144, 19);
            textBoxFirmware.Name = "textBoxFirmware";
            textBoxFirmware.ReadOnly = true;
            textBoxFirmware.Size = new Size(627, 38);
            textBoxFirmware.TabIndex = 1;
            // 
            // buttonSelectFirmware
            // 
            buttonSelectFirmware.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            buttonSelectFirmware.Location = new Point(777, 14);
            buttonSelectFirmware.Name = "buttonSelectFirmware";
            buttonSelectFirmware.Size = new Size(67, 46);
            buttonSelectFirmware.TabIndex = 2;
            buttonSelectFirmware.Text = "...";
            buttonSelectFirmware.UseVisualStyleBackColor = true;
            buttonSelectFirmware.Click += buttonSelectFirmware_Click;
            // 
            // label2
            // 
            label2.AutoSize = true;
            label2.Location = new Point(76, 66);
            label2.Name = "label2";
            label2.Size = new Size(62, 31);
            label2.TabIndex = 3;
            label2.Text = "端口";
            // 
            // comboBoxSerialPort
            // 
            comboBoxSerialPort.FormattingEnabled = true;
            comboBoxSerialPort.Location = new Point(144, 63);
            comboBoxSerialPort.Name = "comboBoxSerialPort";
            comboBoxSerialPort.Size = new Size(179, 39);
            comboBoxSerialPort.TabIndex = 4;
            // 
            // textBoxLog
            // 
            textBoxLog.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            textBoxLog.Location = new Point(28, 108);
            textBoxLog.Multiline = true;
            textBoxLog.Name = "textBoxLog";
            textBoxLog.Size = new Size(816, 533);
            textBoxLog.TabIndex = 5;
            // 
            // progressBar
            // 
            progressBar.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            progressBar.Location = new Point(28, 647);
            progressBar.Name = "progressBar";
            progressBar.Size = new Size(816, 46);
            progressBar.TabIndex = 6;
            // 
            // buttonUpgrade
            // 
            buttonUpgrade.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            buttonUpgrade.Font = new Font("Microsoft YaHei UI", 18F, FontStyle.Regular, GraphicsUnit.Point, 134);
            buttonUpgrade.Location = new Point(28, 699);
            buttonUpgrade.Name = "buttonUpgrade";
            buttonUpgrade.Size = new Size(816, 86);
            buttonUpgrade.TabIndex = 7;
            buttonUpgrade.Text = "升 级";
            buttonUpgrade.UseVisualStyleBackColor = true;
            buttonUpgrade.Click += buttonUpgrade_Click;
            // 
            // Form1
            // 
            AutoScaleDimensions = new SizeF(14F, 31F);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(856, 812);
            Controls.Add(buttonUpgrade);
            Controls.Add(progressBar);
            Controls.Add(textBoxLog);
            Controls.Add(comboBoxSerialPort);
            Controls.Add(label2);
            Controls.Add(buttonSelectFirmware);
            Controls.Add(textBoxFirmware);
            Controls.Add(label1);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            Name = "Form1";
            StartPosition = FormStartPosition.CenterScreen;
            Text = "串行升级工具";
            Load += Form1_Load;
            ResumeLayout(false);
            PerformLayout();
        }

        #endregion

        private Label label1;
        private TextBox textBoxFirmware;
        private Button buttonSelectFirmware;
        private Label label2;
        private ComboBox comboBoxSerialPort;
        private TextBox textBoxLog;
        private ProgressBar progressBar;
        private Button buttonUpgrade;
    }
}
