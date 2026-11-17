using System;
namespace NessStudio.Models
{
    /// <summary>Describes finalization progress without depending on a Dispatcher.</summary>
    public class RecordingSaveProgress
    {
        /// <summary>Gets or sets the operation title.</summary>
        public string Title { get; set; } = "Saving Recording...";
        /// <summary>Gets or sets the current stage message.</summary>
        public string Message { get; set; } = "Preparing finalization...";
        /// <summary>Gets or sets actual progress in percent.</summary>
        public int Percent { get; set; } = 0;
        /// <summary>Gets or sets a value indicating unknown progress.</summary>
        public bool IsIndeterminate { get; set; } = false;
        /// <summary>Gets or sets the current stage number.</summary>
        public int CurrentStep { get; set; } = 0;
        /// <summary>Gets or sets the number of known stages.</summary>
        public int TotalSteps { get; set; } = 0;
    }
}
