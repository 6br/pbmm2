// Author: Armin Töpfer

#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

#include <pbcopper/logging/Logging.h>
#include <pbcopper/utility/FileUtils.h>

#include <pbbam/BamReader.h>
#include <pbbam/BamWriter.h>

#include <pbbam/DataSet.h>
#include <pbbam/DataSetTypes.h>
#include <pbbam/EntireFileQuery.h>
#include <pbbam/PbiFilter.h>
#include <pbbam/PbiFilterQuery.h>

#include <pbcopper/parallel/WorkQueue.h>

#include <Pbmm2Version.h>

#include "AlignSettings.h"
#include "MM2Helper.h"

#include "AlignWorkflow.h"

namespace PacBio {
namespace minimap2 {
namespace {
using FilterFunc = std::function<bool(const BAM::BamRecord&)>;

struct Summary
{
    int32_t NumAlns = 0;
    int64_t Bases = 0;
    double Similarity = 0;
};

void WriteRecords(BAM::BamWriter& out, Summary& s, int64_t& alignedRecords, RecordsType results)
{
    if (!results) return;
    for (const auto& aln : *results) {
        const int32_t span = aln.ReferenceEnd() - aln.ReferenceStart();
        const int32_t nErr = aln.NumDeletedBases() + aln.NumInsertedBases() + aln.NumMismatches();
        s.Bases += span;
        s.Similarity += 1.0 - 1.0 * nErr / span;
        ++s.NumAlns;
        out.Write(aln);
        if (++alignedRecords % 1000 == 0) {
            PBLOG_INFO << "Number of Alignments: " << alignedRecords;
        }
    }
}

void WriterThread(Parallel::WorkQueue<RecordsType>& queue, std::unique_ptr<BAM::BamWriter> out)
{
    Summary s;
    int64_t alignedRecords = 0;
    while (queue.ConsumeWith(WriteRecords, std::ref(*out), std::ref(s), std::ref(alignedRecords))) {
    }
    PBLOG_INFO << "Number of Alignments: " << s.NumAlns;
    PBLOG_INFO << "Number of Bases: " << s.Bases;
    PBLOG_INFO << "Mean Concordance (mapped) : "
               << std::round(1000.0 * s.Similarity / s.NumAlns) / 10.0 << "%";
}

std::tuple<std::string, std::string, std::string> CheckPositionalArgs(
    const std::vector<std::string>& args)
{
    if (args.size() < 2) {
        PBLOG_FATAL << "Please provide at least the input arguments: input reference output!";
        PBLOG_FATAL << "EXAMPLE: pbmm2 input.subreads.bam reference.fasta output.bam";
        std::exit(EXIT_FAILURE);
    }

    const auto inputFile = args[0];
    if (!Utility::FileExists(inputFile)) {
        PBLOG_FATAL << "Input data file does not exist: " << inputFile;
        std::exit(EXIT_FAILURE);
    }
    BAM::DataSet dsInput(inputFile);
    switch (dsInput.Type()) {
        case BAM::DataSet::TypeEnum::SUBREAD:
        case BAM::DataSet::TypeEnum::CONSENSUS_READ:
            break;
        case BAM::DataSet::TypeEnum::ALIGNMENT:
        case BAM::DataSet::TypeEnum::CONSENSUS_ALIGNMENT:
        case BAM::DataSet::TypeEnum::BARCODE:
        case BAM::DataSet::TypeEnum::REFERENCE:
        default:
            PBLOG_FATAL << "Unsupported input data file " << inputFile << " of type "
                        << BAM::DataSet::TypeToName(dsInput.Type());
            std::exit(EXIT_FAILURE);
    }

    const auto& referenceFiles = args[1];
    if (!Utility::FileExists(referenceFiles)) {
        PBLOG_FATAL << "Input reference file does not exist: " << referenceFiles;
        std::exit(EXIT_FAILURE);
    }
    std::string reference;
    if (boost::algorithm::ends_with(referenceFiles, ".mmi")) {
        reference = referenceFiles;
    } else {
        BAM::DataSet dsRef(referenceFiles);
        switch (dsRef.Type()) {
            case BAM::DataSet::TypeEnum::REFERENCE:
                break;
            case BAM::DataSet::TypeEnum::BARCODE:
            case BAM::DataSet::TypeEnum::SUBREAD:
            case BAM::DataSet::TypeEnum::ALIGNMENT:
            case BAM::DataSet::TypeEnum::CONSENSUS_ALIGNMENT:
            case BAM::DataSet::TypeEnum::CONSENSUS_READ:
            default:
                PBLOG_FATAL << "ERROR: Unsupported reference input file " << referenceFiles
                            << " of type " << BAM::DataSet::TypeToName(dsRef.Type());
                std::exit(EXIT_FAILURE);
        }
        const auto fastaFiles = dsRef.FastaFiles();
        if (fastaFiles.size() != 1) {
            PBLOG_FATAL << "Only one reference sequence allowed!";
            std::exit(EXIT_FAILURE);
        }
        reference = fastaFiles.front();
    }

    std::string out;
    if (args.size() == 3)
        out = args[2];
    else
        out = "-";

    return std::tuple<std::string, std::string, std::string>{inputFile, reference, out};
}

std::unique_ptr<BAM::internal::IQuery> BamQuery(const BAM::DataSet& ds)
{
    const auto filter = BAM::PbiFilter::FromDataSet(ds);
    std::unique_ptr<BAM::internal::IQuery> query(nullptr);
    if (filter.IsEmpty())
        query = std::make_unique<BAM::EntireFileQuery>(ds);
    else
        query = std::make_unique<BAM::PbiFilterQuery>(filter, ds);
    return query;
}

void CreateDataSet(const BAM::DataSet& originalInputDataset, const std::string& outputFile,
                   const AlignSettings& settings)
{
    using BAM::DataSet;
    std::string metatype;
    std::string outputType;
    const auto type = originalInputDataset.Type();
    switch (type) {
        case BAM::DataSet::TypeEnum::SUBREAD:
            metatype = "PacBio.AlignmentFile.AlignmentBamFile";
            outputType = "alignmentset";
            break;
        case BAM::DataSet::TypeEnum::CONSENSUS_READ:
            metatype = "PacBio.AlignmentFile.ConsensusAlignmentBamFile";
            outputType = "consensusalignmentset";
            break;
        default:
            throw std::runtime_error("Unsupported input type");
    }
    DataSet ds(type);
    ds.Attribute("xmlns:pbdm") = "http://pacificbiosciences.com/PacBioDataModel.xsd";
    ds.Attribute("xmlns:pbmeta") = "http://pacificbiosciences.com/PacBioCollectionMetadata.xsd";
    ds.Attribute("xmlns:pbpn") = "http://pacificbiosciences.com/PacBioPartNumbers.xsd";
    ds.Attribute("xmlns:pbrk") = "http://pacificbiosciences.com/PacBioReagentKit.xsd";
    ds.Attribute("xmlns:pbsample") = "http://pacificbiosciences.com/PacBioSampleInfo.xsd";
    ds.Attribute("xmlns:pbbase") = "http://pacificbiosciences.com/PacBioBaseDataModel.xsd";

    std::string fileName = outputFile;
    if (fileName.find("/") != std::string::npos) {
        std::vector<std::string> splits;
        boost::split(splits, fileName, boost::is_any_of("/"));
        fileName = splits.back();
    }
    BAM::ExternalResource resource(metatype, fileName + ".bam");

    if (settings.Pbi) {
        BAM::FileIndex pbi("PacBio.Index.PacBioIndex", fileName + ".bam.pbi");
        resource.FileIndices().Add(pbi);
    }

    ds.ExternalResources().Add(resource);

    ds.Name(fileName);
    ds.TimeStampedName(fileName + "-" + BAM::CurrentTimestamp());
    std::ofstream dsOut(outputFile + "." + outputType + ".xml");
    ds.SaveToStream(dsOut);
}

std::string OutputFilePrefix(const std::string& outputFile)
{
    // Check if output type is a dataset
    const std::string outputExt = Utility::FileExtension(outputFile);
    std::string prefix = outputFile;
    if (outputExt == "xml") {
        boost::ireplace_last(prefix, ".consensusalignmentset.xml", "");
        boost::ireplace_last(prefix, ".alignmentset.xml", "");
    } else if (outputExt == "bam") {
        boost::ireplace_last(prefix, ".bam", "");
        boost::ireplace_last(prefix, ".subreads", "");
    } else {
        PBLOG_FATAL << "Unknown file extension for output file: " << outputFile;
        std::exit(EXIT_FAILURE);
    }
    return prefix;
}
}  // namespace

int AlignWorkflow::Runner(const CLI::Results& options)
{
    std::ofstream logStream;
    {
        const std::string logFile = options["log_file"];
        const Logging::LogLevel logLevel(
            options.IsFromRTC() ? options.LogLevel() : options["log_level"].get<std::string>());

        using Logger = PacBio::Logging::Logger;

        Logger* logger;
        if (!logFile.empty()) {
            logStream.open(logFile);
            logger = &Logger::Default(new Logger(logStream, logLevel));
        } else {
            logger = &Logger::Default(new Logger(std::cerr, logLevel));
        }
        PacBio::Logging::InstallSignalHandlers(*logger);
    }

    AlignSettings settings(options);

    BAM::DataSet qryFile;
    std::string refFile;
    std::string outFile;
    std::string alnFile{"-"};

    std::tie(qryFile, refFile, outFile) = CheckPositionalArgs(options.PositionalArguments());

    std::string outputFilePrefix;
    bool outputIsXML = false;
    if (outFile != "-") {
        outputFilePrefix = OutputFilePrefix(outFile);
        outputIsXML = Utility::FileExtension(outFile) == "xml";
        if (outputIsXML)
            alnFile = outputFilePrefix + ".bam";
        else
            alnFile = outFile;

        if (Utility::FileExists(alnFile))
            PBLOG_WARN << "Warning: Overwriting existing output file: " << alnFile;
        if (alnFile != outFile && Utility::FileExists(outFile))
            PBLOG_WARN << "Warning: Overwriting existing output file: " << outFile;
    }

    const FilterFunc filter = [&settings](const BAM::BamRecord& aln) {
        const int32_t span = aln.ReferenceEnd() - aln.ReferenceStart();
        const int32_t nErr = aln.NumDeletedBases() + aln.NumInsertedBases() + aln.NumMismatches();
        if (span <= 0 || span < settings.MinAlignmentLength) return false;
        if (1.0 - 1.0 * nErr / span < settings.MinAccuracy) return false;
        return true;
    };

    MM2Helper mm2helper(refFile, settings.NumThreads);

    {
        auto qryRdr = BamQuery(qryFile);

        const auto bamFiles = qryFile.BamFiles();
        auto hdr = bamFiles.front().Header();
        for (size_t i = 1; i < bamFiles.size(); ++i)
            hdr += bamFiles.at(i).Header();

        if (!settings.SampleName.empty()) {
            auto rgs = hdr.ReadGroups();
            hdr.ClearReadGroups();
            for (auto& rg : rgs) {
                rg.Sample(settings.SampleName);
                hdr.AddReadGroup(rg);
            }
        }

        const auto version = PacBio::Pbmm2Version() + " (commit " + PacBio::Pbmm2GitSha1() + ")";
        for (const auto& si : mm2helper.SequenceInfos())
            hdr.AddSequence(si);
        hdr.AddProgram(
            BAM::ProgramInfo("pbmm2").Name("pbmm2").Version(version).CommandLine(settings.CLI));

        Parallel::WorkQueue<RecordsType> queue(settings.NumThreads);
        auto out = std::make_unique<BAM::BamWriter>(alnFile, hdr);
        auto writer = std::thread(WriterThread, std::ref(queue), std::move(out));

        int32_t i = 0;
        static constexpr const int32_t chunkSize = 100;
        auto records = std::make_unique<std::vector<BAM::BamRecord>>(chunkSize);

        auto Align = [&](const std::unique_ptr<std::vector<BAM::BamRecord>>& recs) -> RecordsType {
            return mm2helper.Align(recs, filter);
        };

        while (qryRdr->GetNext((*records)[i++])) {
            if (i >= chunkSize) {
                queue.ProduceWith(Align, std::move(records));
                records = std::make_unique<std::vector<BAM::BamRecord>>(chunkSize);
                i = 0;
            }
        }
        // terminal records, if they exist
        if (i > 0) {
            records->resize(i);
            queue.ProduceWith(Align, std::move(records));
        }

        queue.Finalize();
        writer.join();
    }

    if (settings.Pbi) {
        BAM::BamFile validationBam(alnFile);
        BAM::PbiFile::CreateFrom(validationBam,
                                 BAM::PbiBuilder::CompressionLevel::CompressionLevel_1,
                                 settings.NumThreads);
    }
    if (outputIsXML) CreateDataSet(qryFile, outputFilePrefix, settings);

    return EXIT_SUCCESS;
}
}  // namespace minimap2
}  // namespace PacBio