using System;
using System.Threading.Tasks;
using NessStudio.Models;

namespace NessStudio.Recording.Engines
{
    /// <summary>Preserves the legacy asynchronous control contract without UI types.</summary>
    /// <remarks>Legacy operations do not support cancellation after admission; callers await real completion before disposal.</remarks>
    public interface IRecorderEngine : IDisposable
    {
        /// <summary>Opens required sources without reporting recording success.</summary>
        Task PrepareAsync();
        /// <summary>Starts the prepared sources on their shared timeline.</summary>
        Task StartAsync();
        /// <summary>Pauses admission of content to the shared timeline.</summary>
        Task PauseAsync();
        /// <summary>Resumes the existing timeline.</summary>
        Task ResumeAsync();
        /// <summary>Stops acquisition and drains the legacy tracks.</summary>
        Task StopAsync();
        /// <summary>Stops sources and awaits real asset and manifest finalization.</summary>
        /// <param name="progress">The optional progress consumer, responsible for frontend marshalling.</param>
        /// <returns>The finalized primary media file; the service result also identifies its session directory.</returns>
        Task<string> StopAndFinalizeAsync(IProgress<RecordingSaveProgress>? progress = null);
    }
}
