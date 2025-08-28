using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.Models
{
    public sealed class ConcatPlan
    {
        public string Prefix { get; }
        public string Ext { get; }
        public IReadOnlyList<string> Parts { get; }
        public string OutputPath { get; }
        public string ListTxtPath { get; }

        public bool NeedsConcat => Parts != null && Parts.Count >= 2;

        private ConcatPlan(string prefix, string ext, IReadOnlyList<string> parts, string outputPath, string listTxtPath)
        {
            Prefix = prefix ?? throw new ArgumentNullException(nameof(prefix));
            Ext = ext ?? throw new ArgumentNullException(nameof(ext));
            Parts = parts ?? throw new ArgumentNullException(nameof(parts));
            OutputPath = outputPath ?? throw new ArgumentNullException(nameof(outputPath));
            ListTxtPath = listTxtPath ?? throw new ArgumentNullException(nameof(listTxtPath));
        }

        public static ConcatPlan Build(NessStudio.Models.RecordingOutputPaths paths, string prefix, string ext)
        {
            var parts = paths.Parts(prefix, ext);
            var output = paths.FinalFile(prefix, ext);
            var list = paths.ConcatListFile(prefix);
            return new ConcatPlan(prefix, ext, parts, output, list);
        }

        public void WriteListFile()
        {
            using var sw = new StreamWriter(ListTxtPath, false, new UTF8Encoding(false));
            foreach (var p in Parts)
            {
                var esc = p.Replace("'", "''");
                sw.WriteLine($"file '{esc}'");
            }
        }
    }
}
