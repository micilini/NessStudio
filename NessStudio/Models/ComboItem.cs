using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.Models
{
    public class ComboItem<T>
    {
        public string Display { get; set; } = string.Empty;
        public T Value { get; set; }
        public override string ToString() => Display;
    }
}
