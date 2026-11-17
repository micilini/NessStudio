using System;
using System.ComponentModel;
using System.Windows;
namespace NessStudio.View.SavingRecordingScreen
{
    public partial class SavingRecordingWindow : Window
    {
        public bool AllowClose { get; set; }
        public event EventHandler? CloseRequestedWhileBusy;
        public SavingRecordingWindow()
        {
            InitializeComponent();
            AllowClose = false;
        }
        protected override void OnClosing(CancelEventArgs e)
        {
            if (!AllowClose)
            {
                e.Cancel = true;
                try
                {
                    CloseRequestedWhileBusy?.Invoke(this, EventArgs.Empty);
                }
                catch
                {
                }
                return;
            }
            base.OnClosing(e);
        }
    }
}