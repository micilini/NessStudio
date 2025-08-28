using System;
using System.ComponentModel;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Forms;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using DirectShowLib;
using NAudio.CoreAudioApi;
using System.Drawing;
using System.Drawing.Imaging;
using OpenCvSharp;
using OpenCvSharp.WpfExtensions;
using NessStudio.ViewModel;

namespace NessStudio.View.RecordingScreen
{
    public partial class RecordingScreenWindow
    {
        public RecordingScreenWindow()
        {
            InitializeComponent();
            this.DataContext = new RecordingScreenWindowVM(this);
        }

    }
}