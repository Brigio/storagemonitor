# storage-monitor-agent

`storage-monitor-agent` is a root system daemon for monitoring Btrfs filesystems and S.M.A.R.T. disk health.

The daemon:

- monitors Btrfs filesystems;
- reads Btrfs device and filesystem information;
- checks S.M.A.R.T. health through `smartctl`;
- stores current state and persistent events in SQLite;
- sends desktop notifications when relevant state changes occur;
- can optionally send email notifications through `msmtp`.

The SQLite database is intended to be the authoritative source of monitoring data. A separate GUI can read the database, but monitoring and event generation remain the responsibility of the daemon.

---

## Requirements

The project requires:

- C++17 or newer;
- CMake;
- Qt 6 Core;
- Qt 6 DBus;
- Qt 6 SQL;
- SQLite Qt driver;
- `btrfs-progs`;
- `smartmontools`.

Optional email notifications require:

- `msmtp`;
- `msmtp-mta`.

On Arch Linux / CachyOS:

```bash
sudo pacman -S cmake qt6-base btrfs-progs smartmontools msmtp msmtp-mta
```

---

## Build

From the project source directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
```

Install with:

```bash
sudo cmake --install build
```

Expected installation paths:

```text
/usr/bin/storage-monitor-agent
/usr/lib/systemd/system/storage-monitor-agent.service
/var/lib/storage-monitor-agent/
/run/storage-monitor-agent/
```

The SQLite database is stored at:

```text
/var/lib/storage-monitor-agent/btrfs.db
```

---

## systemd service

After installation:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now storage-monitor-agent.service
```

Check the service:

```bash
systemctl status storage-monitor-agent.service
```

Follow the daemon log:

```bash
sudo journalctl -u storage-monitor-agent.service -f
```

---

# Email notifications

Email notifications are optional.

`storage-monitor-agent` does not implement SMTP internally. It sends messages through a sendmail-compatible command. The recommended backend is `msmtp`.

```text
Btrfs / SMART event
        |
        +--> Desktop notification
        |
        +--> EmailNotifier
                 |
                 +--> msmtp
                        |
                        +--> SMTP server
```

Email delivery failures do not stop filesystem or S.M.A.R.T. monitoring.

---

## 1. Install msmtp

On Arch Linux / CachyOS:

```bash
sudo pacman -S msmtp msmtp-mta
```

`msmtp-mta` provides sendmail-compatible integration. The agent can also invoke `/usr/bin/msmtp` directly.

Verify:

```bash
which msmtp
```

Expected:

```text
/usr/bin/msmtp
```

---

## 2. Create the agent configuration directory

```bash
sudo mkdir -p /etc/storage-monitor-agent
```

---

## 3. Configure msmtp

Create:

```text
/etc/msmtprc
```

Example for Gmail SMTP:

```ini
defaults
auth           on
tls            on
tls_starttls   on
tls_trust_file /etc/ssl/certs/ca-certificates.crt
logfile        /var/log/msmtp.log

account storage-monitor
host           smtp.gmail.com
port           587
from           your-account@gmail.com
user           your-account@gmail.com
passwordeval   "cat /etc/storage-monitor-agent/msmtp-password"

account default : storage-monitor
```

Protect the file:

```bash
sudo chown root:root /etc/msmtprc
sudo chmod 600 /etc/msmtprc
```

### Gmail note

Use an appropriate SMTP credential for the Gmail account, such as an application password where required. Do not store your normal account password in the project repository.

---

## 4. Store the SMTP password

Create:

```text
/etc/storage-monitor-agent/msmtp-password
```

The file should contain only the SMTP password or application password:

```text
YOUR_SMTP_PASSWORD
```

Protect it:

```bash
sudo chown root:root /etc/storage-monitor-agent/msmtp-password
sudo chmod 600 /etc/storage-monitor-agent/msmtp-password
```

Never commit this file or real SMTP credentials to Git.

---

## 5. Configure storage-monitor-agent email notifications

Create:

```text
/etc/storage-monitor-agent/email.conf
```

Example:

```ini
[Email]

enabled=true

recipient=admin@example.com
sender=your-account@gmail.com

command=/usr/bin/msmtp

timeout_ms=15000
```

Options:

- `enabled`: enables or disables email notifications.
- `recipient`: destination email address.
- `sender`: sender address used in the generated message. It should normally match the `from` address configured in `msmtp`.
- `command`: path to the sendmail-compatible executable. `/usr/bin/msmtp` is recommended.
- `timeout_ms`: maximum time allowed for the mail command to complete.

Protect the configuration file:

```bash
sudo chown root:root /etc/storage-monitor-agent/email.conf
sudo chmod 600 /etc/storage-monitor-agent/email.conf
```

To disable email without deleting the configuration:

```ini
enabled=false
```

---

## 6. Test msmtp before testing the daemon

Always test the SMTP backend independently before troubleshooting `storage-monitor-agent`.

Example:

```bash
printf 'From: your-account@gmail.com\nTo: admin@example.com\nSubject: storage-monitor-agent test\n\nTest email from msmtp.\n' \
    | sudo /usr/bin/msmtp admin@example.com
```

If the message is delivered, the SMTP configuration is working.

Inspect the log with:

```bash
sudo tail -f /var/log/msmtp.log
```

For temporary interactive debugging:

```bash
printf 'From: your-account@gmail.com\nTo: admin@example.com\nSubject: msmtp debug test\n\nDebug test.\n' \
    | sudo /usr/bin/msmtp --debug admin@example.com
```

Be careful with `--debug`, because SMTP diagnostics can expose sensitive information.

---

## 7. Restart storage-monitor-agent

After changing the email configuration:

```bash
sudo systemctl restart storage-monitor-agent.service
```

Then follow the log:

```bash
sudo journalctl -u storage-monitor-agent.service -f
```

When an email notification is sent successfully, the daemon should log a message similar to:

```text
Email notification sent to "admin@example.com"
```

Email delivery failures are logged, but monitoring continues.

---

## Email format

The daemon generates simple plain-text messages.

Example subject:

```text
[cachyos][WARNING] BTRFS Warning
```

Example body:

```text
Storage Monitor Agent

Host: cachyos
Severity: WARNING
Time: 2026-09-17T15:43:31

Event:
BTRFS Warning

Details:
Filesystem bfa50f40: RAID imbalance
```

S.M.A.R.T. notifications use the same format.

Examples:

```text
[cachyos][WARNING] SMART Warning
[cachyos][CRITICAL] SMART CRITICAL
[cachyos][INFO] SMART Recovered
```

The generated message includes standard headers such as:

```text
Message-ID
Date
MIME-Version
Content-Type
Auto-Submitted
X-Mailer
X-Storage-Monitor-Severity
```

---

## Spam filtering

Even correctly authenticated email can occasionally be classified as spam by the destination provider.

When using Gmail SMTP, inspect the received headers and verify that authentication succeeds:

```text
SPF:   PASS
DKIM:  PASS
DMARC: PASS
```

If authentication passes but the message is still classified as spam:

- mark the message as "Not spam";
- add the sender to trusted contacts if supported;
- keep the subject simple;
- avoid unnecessary decorative characters in the subject;
- keep the message body concise and plain text.

The current email format intentionally avoids emoji in the email subject, while desktop notifications can still use them.

---

## S.M.A.R.T. monitoring

S.M.A.R.T. data is collected through:

```bash
smartctl -j -a <device>
```

The daemon stores device state in SQLite and evaluates the result as:

```text
Unknown
OK
Warning
Error
```

Warning conditions can include:

- reallocated sectors;
- pending sectors;
- offline uncorrectable sectors;
- NVMe media errors;
- low NVMe available spare;
- high device temperature.

Critical conditions can include:

- failed SMART overall-health assessment;
- active NVMe critical warning;
- critically high temperature.

Duplicate desktop/email notifications are suppressed while the same health state persists.

State transitions such as:

```text
OK -> Warning
Warning -> Error
Error -> OK
```

can generate notifications.

Recovery notifications are sent when a previously unhealthy device becomes healthy again.

---

## Database

The main database is:

```text
/var/lib/storage-monitor-agent/btrfs.db
```

It contains monitoring state and persistent Btrfs / S.M.A.R.T. events.

The daemon is the writer and authority for monitoring data.

External GUI applications should treat the database as read-only and should not generate monitoring events themselves.

---

## Security notes

`storage-monitor-agent` runs as root because it needs access to storage monitoring information and commands.

Recommended permissions:

```bash
sudo chown root:root /etc/msmtprc
sudo chmod 600 /etc/msmtprc

sudo chown root:root /etc/storage-monitor-agent/email.conf
sudo chmod 600 /etc/storage-monitor-agent/email.conf

sudo chown root:root /etc/storage-monitor-agent/msmtp-password
sudo chmod 600 /etc/storage-monitor-agent/msmtp-password
```

Never commit:

```text
/etc/msmtprc
/etc/storage-monitor-agent/msmtp-password
real SMTP passwords
application passwords
other credentials
```

Only example configurations without credentials should be stored in the repository.

---

## Troubleshooting

### Email is not sent

First test `msmtp` directly:

```bash
printf 'From: your-account@gmail.com\nTo: admin@example.com\nSubject: test\n\nTest.\n' \
    | sudo /usr/bin/msmtp admin@example.com
```

Then check:

```bash
sudo tail -n 50 /var/log/msmtp.log
```

and:

```bash
sudo journalctl -u storage-monitor-agent.service -n 100
```

### msmtp cannot read the password

Check:

```bash
sudo ls -l /etc/storage-monitor-agent/msmtp-password
```

Recommended permissions:

```text
-rw------- root root
```

### The agent cannot find msmtp

Verify:

```bash
ls -l /usr/bin/msmtp
```

and make sure `email.conf` contains:

```ini
command=/usr/bin/msmtp
```

### Email is delivered to spam

Inspect the received message headers and verify SPF, DKIM and DMARC.

If all authentication checks pass, the classification is most likely caused by the destination provider's spam filter rather than by SMTP authentication.

---

## Suggested repository layout

Example configuration files can be stored in the repository:

```text
storage-monitor-agent/
├── README.md
├── config/
│   ├── email.conf.example
│   └── msmtprc.example
└── ...
```

Example files must never contain real email addresses, passwords, tokens or other credentials.

---

## License

This project is licensed under the GNU General Public License v3.0.

See the [LICENSE](LICENSE) file for details.
