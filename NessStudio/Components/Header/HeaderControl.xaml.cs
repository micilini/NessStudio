using NessStudio.ViewModel;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Navigation;
using System.Windows.Shapes;

namespace NessStudio.Components.Header
{
    public partial class HeaderControl : UserControl
    {
        public HomeScreenWindowVM HomeScreenWindowVM { get; set; }
        public HeaderControl(HomeScreenWindowVM HS)
        {
            InitializeComponent();

            HomeScreenWindowVM = HS;
            this.DataContext = new HeaderControlVM(HS);
        }
    }
}
