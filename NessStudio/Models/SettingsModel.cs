using SQLite;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace NessStudio.Models
{
    [Table("Settings")]
    public class SettingsModel
    {

        [PrimaryKey, AutoIncrement]
        public int Id { get; set; }

        public string ApplicationIdentifier { get; set; }

        public string Language { get; set; }

        public DateTime CreatedAt { get; set; }

        public DateTime UpdatedAt { get; set; }

        public SettingsModel()
        {
            ApplicationIdentifier = GenerateApplicationIdentifier();
            Language = "en_us";
            CreatedAt = DateTime.UtcNow;
            UpdatedAt = DateTime.UtcNow;
        }

        private string GenerateApplicationIdentifier()
        {
            const string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            var random = new Random();
            return new string(Enumerable.Repeat(chars, 12).Select(s => s[random.Next(s.Length)]).ToArray());
        }
    }
}
