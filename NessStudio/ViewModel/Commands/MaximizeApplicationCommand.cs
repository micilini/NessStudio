using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Input;

namespace NessStudio.ViewModel.Commands
{
    public class MaximizeApplicationCommand : ICommand
    {

        public HeaderControlVM ViewModel { get; set; }
        public event EventHandler CanExecuteChanged;

        public MaximizeApplicationCommand(HeaderControlVM vm)
        {
            ViewModel = vm;
        }

        public bool CanExecute(object parameter)
        {
            return true;
        }

        public void Execute(object parameter)
        {
            ViewModel.MaximizeApplication();
        }
    }
}
