// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRadioButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <qdesktopservices.h>
#include "common/logging.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/abstract/progress.h"
#include "ui_update_dialog.h"
#include "update_dialog.h"

#include "common/httplib.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

#include <QDesktopServices>

#undef GetSaveFileName

UpdateDialog::UpdateDialog(const Common::Net::Release& release, QWidget* parent)
    : QDialog(parent), ui(new Ui::UpdateDialog) {
    ui->setupUi(this);

    ui->version->setText(
        tr("%1 is available for download.").arg(QString::fromStdString(release.title)));
    ui->url->setText(
        tr("<a href=\"%1\">View on GitHub</a>").arg(QString::fromStdString(release.html_url)));

    std::string text{release.body};
    if (auto pos = text.find("# Packages"); pos != std::string::npos) {
        text = text.substr(0, pos);
    }

    ui->body->setMarkdown(QString::fromStdString(text));

    // TODO(crueter): Find a way to set default
    const auto assets = release.GetPlatformAssets();

    if (assets.empty()) {
        ui->groupBox->setHidden(true);
        connect(this, &QDialog::accepted, this, [release]() {
            QDesktopServices::openUrl(QUrl{QString::fromStdString(release.html_url)});
        });
    } else if (assets.size() == 1) {
        ui->groupBox->setHidden(true);
        m_asset = assets[0];

        connect(this, &QDialog::accepted, this, &UpdateDialog::Download);
    } else {
        u32 i = 0;
        for (const Common::Net::Asset& a : assets) {
            QRadioButton* r = new QRadioButton(tr(a.name.c_str()), this);
            connect(r, &QRadioButton::toggled, this, [a, this](bool checked) {
                if (checked)
                    m_asset = a;
            });

            if (i == 0)
                r->setChecked(true);
            ++i;

            ui->radioButtons->addWidget(r);
        }

        connect(this, &QDialog::accepted, this, &UpdateDialog::Download);
    }
}

UpdateDialog::~UpdateDialog() {
    delete ui;
}

// Downloads the selected asset to `filename` and checks it against the published SHA-256. Nothing
// is left at `filename` unless the whole file arrived and matched.
bool UpdateDialog::DownloadTo(const QString& filename) {
    QSaveFile file(filename);
    if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly)) {
        LOG_WARNING(Frontend, "Could not open file {}", filename.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not open file %1 for writing.").arg(filename));
        return false;
    }

    constexpr std::size_t timeout_seconds = 15;

    httplib::Client client(m_asset.url);
    client.set_connection_timeout(timeout_seconds);
    client.set_read_timeout(timeout_seconds);
    client.set_write_timeout(timeout_seconds);
    client.set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif

    auto progress =
        QtCommon::Frontend::newProgressDialog(tr("Downloading..."), tr("Cancel"), 0, 100);
    progress->show();

    QGuiApplication::processEvents();

    auto progress_callback = [&](size_t processed_size, size_t total_size) {
        QGuiApplication::processEvents();
        if (total_size > 0) {
            progress->setValue(static_cast<int>((processed_size * 100) / total_size));
        }
        return !progress->wasCanceled();
    };

    bool write_failed = false;
    auto content_receiver = [&](const char* chunk, size_t data_length) -> bool {
        if (file.write(chunk, data_length) == -1) {
            LOG_WARNING(Frontend, "Could not write {} bytes to file {}", data_length,
                        filename.toStdString());
            write_failed = true;
            return false;
        }
        return true;
    };

    auto result = client.Get(m_asset.path, content_receiver, progress_callback);
    const bool canceled = progress->wasCanceled();
    progress->close();

    if (write_failed) {
        file.cancelWriting();
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not write to file %1.").arg(filename));
        return false;
    }

    if (!result || result->status >= 400) {
        file.cancelWriting();
        if (canceled) {
            return false;
        }
        const int status = result ? result->status : 0;
        LOG_ERROR(Frontend, "GET to {}{} failed with status {}", m_asset.url, m_asset.path, status);
        QtCommon::Frontend::Critical(tr("Failed to download file"),
                                     tr("Could not download from %1%2\nError code: %3")
                                         .arg(QString::fromStdString(m_asset.url),
                                              QString::fromStdString(m_asset.path),
                                              QString::number(status)));
        return false;
    }

    if (!file.commit()) {
        LOG_WARNING(Frontend, "Could not commit to file {}", filename.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not commit to file %1.").arg(filename));
        return false;
    }

    if (!m_asset.sha256.empty()) {
        QFile saved(filename);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!saved.open(QIODevice::ReadOnly) || !hash.addData(&saved) ||
            hash.result().toHex().toStdString() != m_asset.sha256) {
            saved.close();
            QFile::remove(filename);
            QtCommon::Frontend::Critical(
                tr("Download failed verification"),
                tr("The downloaded file does not match the checksum GitHub published for it, so "
                   "it was discarded."));
            return false;
        }
    }
    return true;
}

// Replaces the running AppImage with the download and offers to restart into it.
void UpdateDialog::InstallInPlace(const QString& target) {
    const QString staged = target + QStringLiteral(".new");
    if (!DownloadTo(staged)) {
        return;
    }

    // The new file keeps the permissions of the old one and must be executable.
    QFile::setPermissions(staged, QFile::permissions(target) | QFile::ExeOwner);
    // Renaming over the running file leaves the running copy intact, since it keeps its inode.
    if (!QFile::remove(target) || !QFile::rename(staged, target)) {
        QFile::remove(staged);
        QtCommon::Frontend::Critical(tr("Failed to install the update"),
                                     tr("Could not replace %1.").arg(target));
        return;
    }

    const auto button =
        QtCommon::Frontend::Question(tr("Update Installed"), tr("Restart now?"),
                                     QtCommon::Frontend::Yes | QtCommon::Frontend::No);
    if (button == QtCommon::Frontend::Yes) {
        // Wait for this process to exit, then start the new version.
        QProcess::startDetached(
            QStringLiteral("/bin/sh"),
            {QStringLiteral("-c"),
             QStringLiteral("while kill -0 %1 2>/dev/null; do sleep 0.2; done; exec \"$0\"")
                 .arg(QCoreApplication::applicationPid()),
             target});
        qApp->quit();
    }
}

void UpdateDialog::Download() {
#ifdef __linux__
    // An AppImage can update itself when its folder is writable.
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    if (m_asset.filename.ends_with(".AppImage") && !appimage.isEmpty() &&
        QFileInfo(QFileInfo(appimage).absolutePath()).isWritable()) {
        InstallInPlace(appimage);
        return;
    }
#endif

    const auto filename = QtCommon::Frontend::GetSaveFileName(
        tr("New Version Location"),
        qApp->applicationDirPath() % QStringLiteral("/") % QString::fromStdString(m_asset.filename),
        tr("All Files (*.*)"));

    if (filename.isEmpty() || !DownloadTo(filename))
        return;

    // Download is complete. User may choose to open in the file manager.
    auto button =
        QtCommon::Frontend::Question(tr("Download Complete"),
                                     tr("Successfully downloaded %1. Would you like to open it?")
                                         .arg(QString::fromStdString(m_asset.filename)),
                                     QtCommon::Frontend::Yes | QtCommon::Frontend::No);

    if (button == QtCommon::Frontend::Yes) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filename));
    }
}
