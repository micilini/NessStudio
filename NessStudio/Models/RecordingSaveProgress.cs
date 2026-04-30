using System;
namespace NessStudio.Models
{
    public class RecordingSaveProgress
    {
        public string Title { get; set; } = "Saving Recording...";
        public string Message { get; set; } = "Preparing finalization...";
        public int Percent { get; set; } = 0;
        public bool IsIndeterminate { get; set; } = false;
        public int CurrentStep { get; set; } = 0;
        public int TotalSteps { get; set; } = 0;
    }
}