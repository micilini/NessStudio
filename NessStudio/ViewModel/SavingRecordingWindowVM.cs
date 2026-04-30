using NessStudio.Models;
using System.ComponentModel;
namespace NessStudio.ViewModel
{
    public class SavingRecordingWindowVM : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        private void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
        private string _title = "Saving Recording...";
        public string Title
        {
            get => _title;
            set
            {
                if (_title != value)
                {
                    _title = value;
                    OnPropertyChanged(nameof(Title));
                }
            }
        }
        private string _message = "Preparing finalization...";
        public string Message
        {
            get => _message;
            set
            {
                if (_message != value)
                {
                    _message = value;
                    OnPropertyChanged(nameof(Message));
                }
            }
        }
        private int _percent = 0;
        public int Percent
        {
            get => _percent;
            set
            {
                if (_percent != value)
                {
                    _percent = value;
                    OnPropertyChanged(nameof(Percent));
                }
            }
        }
        private bool _isIndeterminate = false;
        public bool IsIndeterminate
        {
            get => _isIndeterminate;
            set
            {
                if (_isIndeterminate != value)
                {
                    _isIndeterminate = value;
                    OnPropertyChanged(nameof(IsIndeterminate));
                }
            }
        }
        private int _currentStep = 0;
        public int CurrentStep
        {
            get => _currentStep;
            set
            {
                if (_currentStep != value)
                {
                    _currentStep = value;
                    OnPropertyChanged(nameof(CurrentStep));
                    OnPropertyChanged(nameof(StepText));
                }
            }
        }
        private int _totalSteps = 0;
        public int TotalSteps
        {
            get => _totalSteps;
            set
            {
                if (_totalSteps != value)
                {
                    _totalSteps = value;
                    OnPropertyChanged(nameof(TotalSteps));
                    OnPropertyChanged(nameof(StepText));
                }
            }
        }
        public string StepText
        {
            get
            {
                if (TotalSteps <= 0 || CurrentStep <= 0)
                    return string.Empty;
                return $"Step {CurrentStep} of {TotalSteps}";
            }
        }
        public void ApplyState(RecordingSaveProgress state)
        {
            if (state == null) return;
            Title = state.Title ?? "Saving Recording...";
            Message = state.Message ?? string.Empty;
            Percent = state.Percent;
            IsIndeterminate = state.IsIndeterminate;
            CurrentStep = state.CurrentStep;
            TotalSteps = state.TotalSteps;
        }
    }
}