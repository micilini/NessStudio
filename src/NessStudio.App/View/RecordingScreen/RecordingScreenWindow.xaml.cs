using System;
using NessStudio.ViewModel;
using NessStudio.ViewModel.Helpers;

namespace NessStudio.View.RecordingScreen
{
    public partial class RecordingScreenWindow
    {
        /// <summary>Owns this window's idle sources and independent native presentation channels.</summary>
        public NessStudio.Recording.NativeRecordingWorkspace PreviewWorkspace { get; }
        public RecordingScreenWindow() : this(null) { }
        /// <summary>Composes a recorder explicitly while preserving the application services for presentation and storage.</summary>
        /// <param name="recorderFactory">The recorder composition, or null to use the application policy.</param>
        public RecordingScreenWindow(NessStudio.Recording.Engines.IRecorderEngineFactory? recorderFactory)
        {
            PreviewWorkspace = new NessStudio.Recording.NativeRecordingWorkspace(AppContext.BaseDirectory,DebugLog.Logger);
            InitializeComponent();
            var services = ((App)System.Windows.Application.Current).Services;
            this.DataContext = new RecordingScreenWindowVM(this, recorderFactory ?? services.RecorderFactory, services.RecordingPreferences,
                PreviewWorkspace);
        }
    }
}
