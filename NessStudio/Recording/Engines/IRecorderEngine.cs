using System;
using System.Threading.Tasks;
using NessStudio.Models;

namespace NessStudio.Recording.Engines
{
    public interface IRecorderEngine : IDisposable
    {
        Task PrepareAsync();
        Task StartAsync();
        Task PauseAsync();
        Task ResumeAsync();
        Task StopAsync();
        Task<string> StopAndFinalizeAsync(IProgress<RecordingSaveProgress>? progress = null);
    }
}