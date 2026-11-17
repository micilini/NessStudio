using System.ComponentModel;
namespace NessStudio.Models
{
    public class RecordingPresetOption : INotifyPropertyChanged
    {
        private bool _isSelected;
        public string Label { get; set; }
        public int Value { get; set; }
        public bool IsSelected
        {
            get => _isSelected;
            set
            {
                if (_isSelected == value)
                    return;
                _isSelected = value;
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsSelected)));
            }
        }
        public event PropertyChangedEventHandler PropertyChanged;
    }
}