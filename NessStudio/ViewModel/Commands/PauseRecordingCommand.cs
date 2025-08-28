using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Input;

namespace NessStudio.ViewModel.Commands
{
    public class PauseRecordingCommand : ICommand
    {

        public RecordingScreenWindowVM ViewModel { get; set; }
        public event EventHandler CanExecuteChanged;

        public PauseRecordingCommand(RecordingScreenWindowVM vm)
        {
            ViewModel = vm;
        }

        public bool CanExecute(object parameter)
        {
            return true;
        }

        public void Execute(object parameter)
        {
            ViewModel.HandleButtonAction(MenuAction.PauseRecording);
        }
    }
}
