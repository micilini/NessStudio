using System;
using NessStudio.Models;
using NessStudio.ViewModel.Helpers;
namespace NessStudio.Recording.Engines
{
    public static class RecorderEngineFactory
    {
        public static IRecorderEngine Create(
        RecordingOutputPaths paths,
        RecordingTargets targets,
        System.Windows.Rect? cropPx = null,
        RecordingRuntimeOptions runtimeOptions = null)
        {
            if (paths == null) throw new ArgumentNullException(nameof(paths));
            if (targets == null) throw new ArgumentNullException(nameof(targets));
            return new RecordAssist(paths, targets, cropPx, runtimeOptions);
        }
    }
}