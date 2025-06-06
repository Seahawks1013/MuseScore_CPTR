/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "convertercontroller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>

#include "defer.h"
#include "global/io/file.h"
#include "global/io/dir.h"

#include "convertercodes.h"
#include "compat/backendapi.h"
#include "internal/converterutils.h"

#include "log.h"

using namespace mu::converter;
using namespace mu::project;
using namespace mu::notation;
using namespace muse;
using namespace muse::io;

static const std::string PDF_SUFFIX = "pdf";
static const std::string PNG_SUFFIX = "png";
static const std::string SVG_SUFFIX = "svg";
static const std::string MP3_SUFFIX = "mp3";

Ret ConverterController::batchConvert(const muse::io::path_t& batchJobFile, const muse::io::path_t& stylePath, bool forceMode,
                                      const String& soundProfile, const muse::UriQuery& extensionUri, muse::ProgressPtr progress)
{
    TRACEFUNC;

    if (progress) {
        progress->start();
    }

    RetVal<BatchJob> batchJob = parseBatchJob(batchJobFile);
    if (!batchJob.ret) {
        LOGE() << "failed parse batch job file, err: " << batchJob.ret.toString();
        if (progress) {
            progress->finish(ProgressResult(batchJob.ret));
        }
        return batchJob.ret;
    }

    StringList errors;

    int64_t current = 0;
    int64_t total = batchJob.val.size();
    for (const Job& job : batchJob.val) {
        if (progress) {
            ++current;
            progress->progress(current, total, job.in.toStdString());
        }

        Ret ret = fileConvert(job.in, job.out, stylePath, forceMode, soundProfile, extensionUri, job.transposeOptions);
        if (!ret) {
            errors.emplace_back(String(u"failed convert, err: %1, in: %2, out: %3")
                                .arg(String::fromStdString(ret.toString())).arg(job.in.toString()).arg(job.out.toString()));
        }
    }

    Ret ret;
    if (!errors.empty()) {
        ret = make_ret(Err::ConvertFailed, errors.join(u"\n").toStdString());
    } else {
        ret = make_ret(Ret::Code::Ok);
    }

    if (progress) {
        progress->finish(ProgressResult(ret));
    }

    return ret;
}

Ret ConverterController::fileConvert(const muse::io::path_t& in, const muse::io::path_t& out,
                                     const muse::io::path_t& stylePath,
                                     bool forceMode,
                                     const muse::String& soundProfile,
                                     const muse::UriQuery& extensionUri,
                                     const std::string& transposeOptionsJson)
{
    std::optional<TransposeOptions> transposeOptions;

    if (!transposeOptionsJson.empty()) {
        RetVal<TransposeOptions> transposeOptionsRet = ConverterUtils::parseTransposeOptions(transposeOptionsJson);
        if (!transposeOptionsRet.ret) {
            return transposeOptionsRet.ret;
        }

        transposeOptions = transposeOptionsRet.val;
    }

    return fileConvert(in, out, stylePath, forceMode, soundProfile, extensionUri, transposeOptions);
}

Ret ConverterController::fileConvert(const muse::io::path_t& in, const muse::io::path_t& out,
                                     const muse::io::path_t& stylePath,
                                     bool forceMode,
                                     const String& soundProfile,
                                     const muse::UriQuery& extensionUri,
                                     const std::optional<notation::TransposeOptions>& transposeOptions)
{
    TRACEFUNC;

    LOGI() << "in: " << in << ", out: " << out;

    std::string suffix = io::suffix(out);

    auto writer = writers()->writer(suffix);
    if (!writer) {
        return make_ret(Err::ConvertTypeUnknown);
    }

    auto notationProject = notationCreator()->newProject(iocContext());
    IF_ASSERT_FAILED(notationProject) {
        return make_ret(Err::UnknownError);
    }

    Ret ret = notationProject->load(in, stylePath, forceMode);
    if (!ret) {
        LOGE() << "failed load notation, err: " << ret.toString() << ", path: " << in;
        return make_ret(Err::InFileFailedLoad);
    }

    if (!soundProfile.isEmpty()) {
        notationProject->audioSettings()->clearTrackInputParams();
        notationProject->audioSettings()->setActiveSoundProfile(soundProfile);
    }

    if (transposeOptions.has_value()) {
        ret = ConverterUtils::applyTranspose(notationProject->masterNotation()->notation(), transposeOptions.value());
        if (!ret) {
            LOGE() << "Failed to apply transposition, err: " << ret.toString();
            return ret;
        }
    }

    globalContext()->setCurrentProject(notationProject);

    DEFER {
        globalContext()->setCurrentProject(nullptr);
    };

    // Check if this is a part conversion job
    QString baseName = QString::fromStdString(io::completeBasename(out).toStdString());
    if (baseName.contains('*')) {
        return convertScoreParts(writer, notationProject->masterNotation(), out);
    }

    // use a extension for convert
    if (extensionUri.isValid()) {
        ret = convertByExtension(writer, notationProject->masterNotation()->notation(), out, extensionUri);
        if (!ret) {
            LOGE() << "Failed to convert by extension, err: " << ret.toString();
        }
    }
    // standart convert
    else {
        if (suffix == engraving::MSCZ || suffix == engraving::MSCX || suffix == engraving::MSCS) {
            return notationProject->save(out);
        }

        if (isConvertPageByPage(suffix)) {
            ret = convertPageByPage(writer, notationProject->masterNotation()->notation(), out);
            if (!ret) {
                LOGE() << "Failed to convert page by page, err: " << ret.toString();
            }
        } else {
            ret = convertFullNotation(writer, notationProject->masterNotation()->notation(), out);
            if (!ret) {
                LOGE() << "Failed to convert full notation, err: " << ret.toString();
            }
        }
    }

    return ret;
}

Ret ConverterController::convertScoreParts(const muse::io::path_t& in, const muse::io::path_t& out, const muse::io::path_t& stylePath,
                                           bool forceMode)
{
    TRACEFUNC;

    auto notationProject = notationCreator()->newProject(iocContext());
    IF_ASSERT_FAILED(notationProject) {
        return make_ret(Err::UnknownError);
    }

    std::string suffix = io::suffix(out);
    auto writer = writers()->writer(suffix);
    if (!writer) {
        return make_ret(Err::ConvertTypeUnknown);
    }

    Ret ret = notationProject->load(in, stylePath, forceMode);
    if (!ret) {
        LOGE() << "failed load notation, err: " << ret.toString() << ", path: " << in;
        return make_ret(Err::InFileFailedLoad);
    }

    ret = convertScoreParts(writer, notationProject->masterNotation(), out);

    return ret;
}

Ret ConverterController::convertScoreParts(INotationWriterPtr writer, IMasterNotationPtr masterNotation, const muse::io::path_t& out)
{
    std::string suffix = io::suffix(out);

    if (suffix == PDF_SUFFIX) {
        return convertScorePartsToPdf(writer, masterNotation, out);
    } else if (suffix == PNG_SUFFIX) {
        return convertScorePartsToPngs(writer, masterNotation, out);
    } else if (suffix == MP3_SUFFIX) {
        return convertScorePartsToMp3(writer, masterNotation, out);
    }

    return make_ret(Ret::Code::NotSupported);
}

RetVal<ConverterController::BatchJob> ConverterController::parseBatchJob(const muse::io::path_t& batchJobFile) const
{
    TRACEFUNC;

    RetVal<BatchJob> rv;
    QFile file(batchJobFile.toQString());
    if (!file.open(QIODevice::ReadOnly)) {
        rv.ret = make_ret(Err::BatchJobFileFailedOpen);
        return rv;
    }

    const QByteArray data = file.readAll();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        rv.ret = make_ret(Err::BatchJobFileFailedParse, err.errorString().toStdString());
        return rv;
    }

    const QJsonArray arr = doc.array();

    auto correctUserInputPath = [](const QString& path) -> QString {
        return io::Dir::fromNativeSeparators(path).toQString();
    };

    for (const auto obj : arr) {
        Job job;
        job.in = correctUserInputPath(obj[u"in"].toString());

        QJsonObject transposeOptionsObj = obj[u"transpose"].toObject();
        if (!transposeOptionsObj.isEmpty()) {
            RetVal<TransposeOptions> transposeOptions = ConverterUtils::parseTransposeOptions(transposeOptionsObj);
            if (!transposeOptions.ret) {
                rv.ret = transposeOptions.ret;
                return rv;
            }
            job.transposeOptions = transposeOptions.val;
        }

        const QJsonValue outValue = obj[u"out"];
        if (outValue.isString()) {
            job.out = correctUserInputPath(outValue.toString());
            rv.val.push_back(std::move(job));
        } else if (outValue.isArray()) {
            const QJsonArray outArray = outValue.toArray();
            for (const auto outItem : outArray) {
                Job partJob = job; // Copy the input path
                if (outItem.isString()) {
                    partJob.out = correctUserInputPath(outItem.toString());
                } else if (outItem.isArray() && outItem.toArray().size() == 2) {
                    const QJsonArray partOutArray = outItem.toArray();
                    const QString prefix = correctUserInputPath(partOutArray[0].toString());
                    const QString suffix = partOutArray[1].toString();
                    partJob.out = prefix + "*" + suffix; // Use "*" as a placeholder for part names
                }
                rv.val.push_back(std::move(partJob));
            }
        }
    }

    rv.ret = make_ret(Ret::Code::Ok);
    return rv;
}

Ret ConverterController::convertByExtension(INotationWriterPtr writer, INotationPtr notation, const muse::io::path_t& out,
                                            const muse::UriQuery& extensionUri)
{
    //! NOTE First we do the extension, it can modify the notation (score)
    Ret ret = extensionsProvider()->perform(extensionUri);
    if (!ret) {
        return ret;
    }

    File file(out);
    if (!file.open(File::WriteOnly)) {
        return make_ret(Err::OutFileFailedOpen);
    }

    file.setMeta("file_path", out.toStdString());
    ret = writer->write(notation, file);
    if (!ret) {
        LOGE() << "failed write, err: " << ret.toString() << ", path: " << out;
        return make_ret(Err::OutFileFailedWrite);
    }

    file.close();

    return make_ret(Ret::Code::Ok);
}

bool ConverterController::isConvertPageByPage(const std::string& suffix) const
{
    QList<std::string> types {
        PNG_SUFFIX,
        SVG_SUFFIX
    };

    return types.contains(suffix);
}

Ret ConverterController::convertPageByPage(INotationWriterPtr writer, INotationPtr notation, const muse::io::path_t& out) const
{
    TRACEFUNC;

    for (size_t i = 0; i < notation->elements()->pages().size(); i++) {
        const String filePath = muse::io::path_t(io::dirpath(out) + "/"
                                                 + io::completeBasename(out) + "-%1."
                                                 + io::suffix(out)).toString().arg(i + 1);

        File file(filePath);
        if (!file.open(File::WriteOnly)) {
            return make_ret(Err::OutFileFailedOpen);
        }

        INotationWriter::Options options = {
            { INotationWriter::OptionKey::PAGE_NUMBER, Val(static_cast<int>(i)) },
        };

        file.setMeta("dir_path", out.toStdString());
        file.setMeta("file_path", filePath.toStdString());

        Ret ret = writer->write(notation, file, options);
        if (!ret) {
            LOGE() << "failed write, err: " << ret.toString() << ", path: " << out;
            return make_ret(Err::OutFileFailedWrite);
        }

        file.close();
    }

    return make_ret(Ret::Code::Ok);
}

Ret ConverterController::convertFullNotation(INotationWriterPtr writer, INotationPtr notation, const muse::io::path_t& out) const
{
    File file(out);
    if (!file.open(File::WriteOnly)) {
        return make_ret(Err::OutFileFailedOpen);
    }

    file.setMeta("file_path", out.toStdString());
    Ret ret = writer->write(notation, file);
    if (!ret) {
        LOGE() << "failed write, err: " << ret.toString() << ", path: " << out;
        return make_ret(Err::OutFileFailedWrite);
    }

    file.close();

    return make_ret(Ret::Code::Ok);
}

Ret ConverterController::convertScorePartsToPdf(INotationWriterPtr writer, IMasterNotationPtr masterNotation,
                                                const muse::io::path_t& out) const
{
    TRACEFUNC;

    INotationPtrList notations;
    for (const IExcerptNotationPtr& e : masterNotation->excerpts()) {
        notations.push_back(e->notation());
    }

    for (size_t i = 0; i < notations.size(); ++i) {
        QString partName = notations[i]->name();
        QString baseName = QString::fromStdString(io::completeBasename(out).toStdString());
        muse::io::path_t partOut = io::dirpath(out) + "/" + baseName.replace("*", partName).toStdString() + ".pdf";

        File file(partOut);
        if (!file.open(File::WriteOnly)) {
            return make_ret(Err::OutFileFailedOpen);
        }

        INotationWriter::Options options {
            { INotationWriter::OptionKey::UNIT_TYPE, Val(INotationWriter::UnitType::PER_PART) },
        };

        Ret ret = writer->write(notations[i], file, options);
        if (!ret) {
            LOGE() << "failed write, err: " << ret.toString() << ", path: " << partOut;
            return make_ret(Err::OutFileFailedWrite);
        }

        file.close();
    }

    return make_ret(Ret::Code::Ok);
}

Ret ConverterController::convertScorePartsToPngs(INotationWriterPtr writer, mu::notation::IMasterNotationPtr masterNotation,
                                                 const muse::io::path_t& out) const
{
    TRACEFUNC;

    INotationPtrList notations;
    for (const IExcerptNotationPtr& e : masterNotation->excerpts()) {
        notations.push_back(e->notation());
    }

    for (size_t i = 0; i < notations.size(); i++) {
        QString partName = notations[i]->name();
        QString baseName = QString::fromStdString(io::completeBasename(out).toStdString());
        muse::io::path_t pngFilePath = io::dirpath(out) + "/" + baseName.replace("*", partName).toStdString() + ".png";
        Ret ret2 = convertPageByPage(writer, notations[i], pngFilePath);
        if (!ret2) {
            return ret2;
        }
    }

    return make_ret(Ret::Code::Ok);
}

Ret ConverterController::convertScorePartsToMp3(INotationWriterPtr writer, IMasterNotationPtr masterNotation,
                                                const muse::io::path_t& out) const
{
    TRACEFUNC;

    INotationPtrList notations;
    for (const IExcerptNotationPtr& e : masterNotation->excerpts()) {
        notations.push_back(e->notation());
    }

    for (size_t i = 0; i < notations.size(); ++i) {
        QString partName = notations[i]->name();
        QString baseName = QString::fromStdString(io::completeBasename(out).toStdString());
        muse::io::path_t partOut = io::dirpath(out) + "/" + baseName.replace("*", partName).toStdString() + ".mp3";

        File file(partOut);
        if (!file.open(File::WriteOnly)) {
            return make_ret(Err::OutFileFailedOpen);
        }

        INotationWriter::Options options {
            { INotationWriter::OptionKey::UNIT_TYPE, Val(INotationWriter::UnitType::PER_PART) },
        };
        file.setMeta("file_path", partOut.toStdString());
        Ret ret = writer->write(notations[i], file, options);
        if (!ret) {
            LOGE() << "failed write, err: " << ret.toString() << ", path: " << partOut;
            return make_ret(Err::OutFileFailedWrite);
        }

        file.close();
    }

    return make_ret(Ret::Code::Ok);
}

Ret ConverterController::exportScoreMedia(const muse::io::path_t& in, const muse::io::path_t& out,
                                          const muse::io::path_t& highlightConfigPath,
                                          const muse::io::path_t& stylePath, bool forceMode)
{
    TRACEFUNC;

    return BackendApi::exportScoreMedia(in, out, highlightConfigPath, stylePath, forceMode);
}

Ret ConverterController::exportScoreMeta(const muse::io::path_t& in, const muse::io::path_t& out, const muse::io::path_t& stylePath,
                                         bool forceMode)
{
    TRACEFUNC;

    return BackendApi::exportScoreMeta(in, out, stylePath, forceMode);
}

Ret ConverterController::exportScoreParts(const muse::io::path_t& in, const muse::io::path_t& out, const muse::io::path_t& stylePath,
                                          bool forceMode)
{
    TRACEFUNC;

    return BackendApi::exportScoreParts(in, out, stylePath, forceMode);
}

Ret ConverterController::exportScorePartsPdfs(const muse::io::path_t& in, const muse::io::path_t& out, const muse::io::path_t& stylePath,
                                              bool forceMode)
{
    TRACEFUNC;

    return BackendApi::exportScorePartsPdfs(in, out, stylePath, forceMode);
}

Ret ConverterController::exportScoreTranspose(const muse::io::path_t& in, const muse::io::path_t& out, const std::string& optionsJson,
                                              const muse::io::path_t& stylePath, bool forceMode)
{
    TRACEFUNC;

    return BackendApi::exportScoreTranspose(in, out, optionsJson, stylePath, forceMode);
}

Ret ConverterController::exportScoreVideo(const muse::io::path_t& in, const muse::io::path_t& out)
{
    TRACEFUNC;

    auto notationProject = notationCreator()->newProject(iocContext());
    IF_ASSERT_FAILED(notationProject) {
        return make_ret(Err::UnknownError);
    }

    std::string suffix = io::suffix(out);
    auto writer = projectRW()->writer(suffix);
    if (!writer) {
        return make_ret(Err::ConvertTypeUnknown);
    }

    Ret ret = notationProject->load(in);
    if (!ret) {
        LOGE() << "failed load notation, err: " << ret.toString() << ", path: " << in;
        return make_ret(Err::InFileFailedLoad);
    }

    ret = writer->write(notationProject, out);
    if (!ret) {
        LOGE() << "failed write, err: " << ret.toString() << ", path: " << out;
        return make_ret(Err::OutFileFailedWrite);
    }

    return make_ret(Ret::Code::Ok);
}

Ret ConverterController::updateSource(const muse::io::path_t& in, const std::string& newSource, bool forceMode)
{
    TRACEFUNC;

    return BackendApi::updateSource(in, newSource, forceMode);
}
