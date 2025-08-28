using NessStudio.Models;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Input;

namespace NessStudio.ViewModel.Commands
{
    public class StartRecordingCommand : ICommand
    {

        public MenuControlVM ViewModel { get; set; }
        public event EventHandler CanExecuteChanged;

        public StartRecordingCommand(MenuControlVM vm)
        {
            ViewModel = vm;
        }

        public bool CanExecute(object parameter)
        {
            return true;
        }

        public void Execute(object parameter)
        {
            ViewModel.HandleButtonAction(MenuAction.StartRecording);
        }
    }
}
