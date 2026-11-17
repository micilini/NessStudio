using System.Windows.Media;
using NessStudio.Models;

namespace NessStudio.Presentation;

/// <summary>Identifies the frontend presentation form, independently of the recording backend.</summary>
public enum PreviewPresentationKind { CpuBitmap, GpuSurface }

/// <summary>Transfers immutable frontend presentation, never recording pixels through Core.</summary>
public sealed record PreviewPresentation(ImageSource Image, PreviewPresentationKind Kind, long Generation);

/// <summary>Reports actual legacy preview costs separately from recording and UI composition.</summary>
public sealed record ScreenPreviewStatistics(long Captured, long Presented, long Dropped,
    TimeSpan CaptureWallTime, TimeSpan? CaptureThreadCpuTime, int WorkerThreadId, int PresentationThreadId);

/// <summary>Controls the temporary preview; notifications arrive on the owning Dispatcher.</summary>
public interface IScreenPreviewService : IAsyncDisposable
{
    event EventHandler<PreviewPresentation>? FrameReady;
    event EventHandler<Exception>? Failed;
    ScreenPreviewStatistics Statistics { get; }
    void Start(MonitorDescriptor monitor);
    void CaptureOnce(MonitorDescriptor monitor);
    void Stop();
}
