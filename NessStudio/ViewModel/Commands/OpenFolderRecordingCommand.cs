using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Input;

namespace NessStudio.ViewModel.Commands
{
    public class OpenFolderRecordingCommand : ICommand
    {
        public RecentProjectsControlVM ViewModel { get; set; }
        public event EventHandler CanExecuteChanged;

        public OpenFolderRecordingCommand(RecentProjectsControlVM vm)
        {
            ViewModel = vm;
        }

        public bool CanExecute(object parameter)
        {
            return true;
        }

        public void Execute(object parameter)
        {
            ViewModel.OpenFolder_Click();
        }
    }
}
