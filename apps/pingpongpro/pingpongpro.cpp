// ==========================================================================
//				pingpongpro
// ==========================================================================
// todo: copyright

#include <seqan/basic.h>
#include <seqan/sequence.h>
#include <seqan/bam_io.h>
#include <seqan/bed_io.h>
#include <seqan/arg_parse.h>

#include <map>
#include <vector>
#include <ctime>
#include <fstream>
#include <cmath>

using namespace std;
using namespace seqan;

// ==========================================================================
// Types & Classes
// ==========================================================================

#if defined(WIN32) || defined(_WIN32) 
#define PATH_SEPARATOR '\\' 
#else 
#define PATH_SEPARATOR '/'
#endif

// type to hold the list of input files given as arguments to the program
typedef vector<CharString> TInputFiles;

enum TCountMultiHits { multiHitsWeighted, multiHitsDiscard, multiHitsUnique };

// struct to store the options from the command line
struct AppOptions
{
	bool bedGraph;
	TInputFiles inputFiles;
	unsigned int minReadLength;
	unsigned int maxReadLength;
	unsigned int minStackHeight;
	TCountMultiHits countMultiHits;
	CharString output;
	bool plot;
	unsigned int verbosity;
};

// types to store @SQ header lines of BAM/SAM files
typedef StringSet<CharString> TNameStore;
typedef Iterator<TNameStore>::Type TNameStoreIterator;

// constants to refer to + and - strands throughout the program
const unsigned int STRAND_PLUS = 0;
const unsigned int STRAND_MINUS = 1;

// for every locus (position) on the genome the following attributes are calculated:
//  - reads: the number of reads which begin at this position
//  - UAt5PrimeEnd: whether the reads of the stack have a U at the 5' end
struct TCountsPosition
{
	float reads;
	bool UAt5PrimeEnd;
	TCountsPosition():
		reads(0),
		UAt5PrimeEnd(false)
	{}
};

// The following types define nested arrays to store the above stats for every position in the genome.
// The stats are grouped by strand and contig/chromosome.
typedef map< unsigned int, TCountsPosition > TCountsContig;
typedef map< unsigned int, TCountsContig > TCountsStrand;
typedef TCountsStrand TCountsGenome[2];

// true ping-pong stacks overlap by this many nt
#define PING_PONG_OVERLAP 10

// stacks with overlaps between MIN_ARBITRARY_OVERLAP and MAX_ARBITRARY_OVERLAP (except for PING_PONG_OVERLAP)
// are used to estimate what is background noise
#define MIN_ARBITRARY_OVERLAP 0
#define MAX_ARBITRARY_OVERLAP 20

// type to store a score for each stack height found in the input file
typedef map< unsigned int, float > THeightScoreMap;

// todo: description
#define IS_URIDINE 0
#define IS_NOT_URIDINE 1
#define IS_ABOVE_COVERAGE 0
#define IS_BELOW_COVERAGE 1
#define HEIGHT_SCORE_BINS 1000
typedef vector< vector< vector< vector< float > > > > TGroupedStackCounts;
typedef vector< TGroupedStackCounts > TGroupedStackCountsByOverlap;

// the probability of having uridine at the 5' end of reads (for non-piRNA data)
#define URIDINE_PROBABILITY 0.25

// todo: description
struct TPingPongOverlap
{
	unsigned int heightScoreBin: 29; // must be big enough to hold <HEIGHT_SCORE_BINS>
	unsigned int localHeightScoreBin: 1; // holds either <IS_ABOVE_COVERAGE> or <IS_BELOW_COVERAGE>
	unsigned int UAt5PrimeEndOnPlusStrandBin: 1; // holds either <IS_URIDINE> or <IS_NOT_URIDINE>
	unsigned int UAt5PrimeEndOnMinusStrandBin: 1; // holds either <IS_URIDINE> or <IS_NOT_URIDINE>
};

// ==========================================================================
// Functions
// ==========================================================================

// function to parse command-line arguments
ArgumentParser::ParseResult parseCommandLine(AppOptions &options, int argc, char const ** argv)
{
	ArgumentParser parser("pingpongpro");

	// define usage and description
	addUsageLine(parser, "[\\fIOPTIONS\\fP] [-i \\fISAM_INPUT_FILE\\fP [-i ...]] [-o \\fIOUTPUT_DIRECTORY\\fP]");
	setShortDescription(parser, "Find ping-pong signatures like a pro");
	// todo: define long description
	addDescription(parser, "PingPongPro scans piRNA-Seq data for signs of ping-pong cycle activity. The ping-pong cycle produces piRNA molecules with complementary 5'-ends. These molecules appear as stacks of aligned reads whose 5'-ends overlap with the 5'-ends of reads on the opposite strand by exactly 10 bases.");
	setVersion(parser, "0.1");
	setDate(parser, "Mar 2014");

	addOption(parser, ArgParseOption("b", "bedgraph", "Output loci with ping-pong signature in bedGraph format. Default: off."));

	addOption(parser, ArgParseOption("s", "min-stack-height", "Omit stacks with fewer than the specified number of reads from the output.", ArgParseArgument::INTEGER, "NUMBER_OF_READS", true));
	setDefaultValue(parser, "min-stack-height", 1);
	setMinValue(parser, "min-stack-height", "1");

	addOption(parser, ArgParseOption("i", "input", "Input file(s) in SAM/BAM format. \"-\" means stdin.", ArgParseArgument::INPUTFILE, "PATH", true));
	setDefaultValue(parser, "input", "-");
	setValidValues(parser, "input", ".bam .sam -");

	addOption(parser, ArgParseOption("l", "min-read-length", "Ignore reads in the input file that are shorter than the specified length.", ArgParseArgument::INTEGER, "LENGTH", true));
	setDefaultValue(parser, "min-read-length", 1);
	setMinValue(parser, "min-read-length", "1");
	addOption(parser, ArgParseOption("L", "max-read-length", "Ignore reads in the input file that are longer than the specified length.", ArgParseArgument::INTEGER, "LENGTH", true));
	setDefaultValue(parser, "max-read-length", 1000);
	setMinValue(parser, "max-read-length", "1");

	addOption(parser, ArgParseOption("m", "multihits", "How to count multi-mapping reads.", ArgParseArgument::STRING, "METHOD", true));
	setDefaultValue(parser, "multihits", "weighted");
	setValidValues(parser, "multihits", "weighted discard unique");

	addOption(parser, ArgParseOption("o", "output", "Write output to specified directory. Default: current working directory.", ArgParseArgument::OUTPUTFILE, "PATH", true));

	addOption(parser, ArgParseOption("p", "plot", "Generate R plots on background noise estimation. Requires Rscript. Default: off."));

	addOption(parser, ArgParseOption("v", "verbose", "Print messages to stderr about the current progress. Default: off."));

	// parse command line
	ArgumentParser::ParseResult parserResult = parse(parser, argc, argv);
	if (parserResult != ArgumentParser::PARSE_OK)
		return parserResult;

	// extract options, if parsing was successful
	options.bedGraph = isSet(parser, "bedgraph");
	options.inputFiles.resize(getOptionValueCount(parser, "input")); // store input files in vector
	if (options.inputFiles.size() == 0)
	{
		options.inputFiles.push_back("/dev/stdin"); // read from stdin, if no input file is given
	}
	else
	{
		for (vector< string >::size_type i = 0; i < options.inputFiles.size(); i++)
		{
			getOptionValue(options.inputFiles[i], parser, "input", i);
			if (options.inputFiles[i] == "-")
				options.inputFiles[i] = "/dev/stdin";
		}
	}
	string countMultiHits;
	getOptionValue(countMultiHits, parser, "multihits");
	if (countMultiHits == "unique")
	{
		options.countMultiHits = multiHitsUnique;
	}
	else if (countMultiHits == "discard")
	{
		options.countMultiHits = multiHitsDiscard;
	}
	else
	{
		options.countMultiHits = multiHitsWeighted;
	}
	getOptionValue(options.minStackHeight, parser, "min-stack-height");
	getOptionValue(options.minReadLength, parser, "min-read-length");
	getOptionValue(options.maxReadLength, parser, "max-read-length");
	if (options.minReadLength > options.maxReadLength)
	{
		cerr << getAppName(parser) << ": maximum read length (" << options.maxReadLength << ") must not be lower than minimum read length (" << options.minReadLength << ")" << endl;
		return ArgumentParser::PARSE_ERROR;
	}
	getOptionValue(options.output, parser, "output");
	if ((length(options.output) > 0) && (options.output[length(options.output)-1] != PATH_SEPARATOR))
		options.output += PATH_SEPARATOR; // append slash to output path, if missing
	options.plot = isSet(parser, "plot");
	if (isSet(parser, "verbose"))
	{
		options.verbosity = 3;
	}
	else
	{
		options.verbosity = 0;
	}

	return parserResult;
}

// function to measure time between the first and second invocation of the function
unsigned int stopwatch(const string &operation, unsigned int verbosity)
{
	static time_t start = 0;
	unsigned int elapsedSeconds = 0;
	if (start != 0)
	{
		elapsedSeconds = time(NULL) - start;
		start = 0;
	}
	else
	{
		start = time(NULL);
	}
	if (verbosity >= 3)
	{
		if (elapsedSeconds > 0)
			cerr << "done (" << elapsedSeconds << " seconds)" << endl;
		else
			cerr << operation << " ... ";
	}
	cerr.flush();
	return elapsedSeconds;
}
unsigned int stopwatch(unsigned int verbosity)
{
	return stopwatch("", verbosity);
}

// Function which sums up the number of reads that start at a given position in the genome.
// Additionally, it counts the number of reads with uridine at the 5' end.
// Parameters:
//   bamFile: the BAM/SAM file from where to load the reads
//   readCounts: stats for positions were reads on the minus strand overlap the 5' ends of reads on the plus strand
// todo: document parameters
int countReadsInBamFile(BamStream &bamFile, TCountsGenome &readCounts, unsigned int minReadLength, unsigned int maxReadLength, TCountMultiHits countMultiHits)
{
	TCountsPosition *position;

	BamAlignmentRecord record;
	while (!atEnd(bamFile))
	{
		if (readRecord(record, bamFile) != 0)
		{
			cerr << "Failed to read record" << endl;
			return 1;
		}

		if ((record.beginPos != BamAlignmentRecord::INVALID_POS) && (record.beginPos != -1) && // skip unmapped reads
		    (length(record.seq) >= minReadLength) && (length(record.seq) <= maxReadLength)) // skip reads which are not within the specified length range

		{
			// the stack height is increased by the value of this variable (depends on how multi-hits are handled)
			float readWeight;

			if (countMultiHits == multiHitsUnique) // we do not distinguish between multi-hits and unique hits
			{
				// increase stack height by 1, regardless of whether it is a multi-hit or unique hit
				readWeight = 1;
			}
			else
			{
				// check if the record is a multi-hit by examining the optional tags
				BamTagsDict tagsDictionary(record.tags);
				unsigned int tagIndex;
				unsigned int multiHits = 1;
				if (findTagKey(tagIndex, tagsDictionary, "NH"))
					extractTagValue(multiHits, tagsDictionary, tagIndex);

				if (countMultiHits == multiHitsWeighted)
				{
					// increase stack height by fraction
					readWeight = 1.0 / multiHits;
				}
				else /*if (countMultiHits == multiHitsDiscard)*/
				{
					// discard read (i.e., set readWeight to 0), if there is more than 1 instance in the SAM file
					readWeight = (multiHits == 1) ? 1 : 0;
				}
			}

			if (readWeight <= 0)
				continue; // skip to next read, if read is to be discarded

			if (hasFlagRC(record)) // read maps to minus strand
			{
				// calculate length of alignment using CIGAR string
				size_t alignmentLength = 0;
				for (unsigned int cigarIndex = 0; cigarIndex < length(record.cigar); ++cigarIndex)
				{
					if ((record.cigar[cigarIndex].operation == 'M') || (record.cigar[cigarIndex].operation == 'N') || (record.cigar[cigarIndex].operation == 'D') || (record.cigar[cigarIndex].operation == '=') || (record.cigar[cigarIndex].operation == 'X')) // these CIGAR elements indicate alignment
						alignmentLength += record.cigar[cigarIndex].count;
				}

				// get a pointer to counter of the position of the read
				position = &(readCounts[STRAND_MINUS][record.rID][record.beginPos+alignmentLength]);

				// check if base at 5' end is uridine
				size_t clippedBasesAt5PrimeEnd = 0;
				if ((length(record.cigar) > 1) && (record.cigar[length(record.cigar)-1].operation == 'S'))
					clippedBasesAt5PrimeEnd = record.cigar[length(record.cigar)-1].count;
				if ((record.seq[length(record.seq)-clippedBasesAt5PrimeEnd-1] == 'A') || (record.seq[length(record.seq)-clippedBasesAt5PrimeEnd-1] == 'a')) // check if last base is uridine (we check for adenine, because reads on the - strand are stored as the complement in SAM files
					position->UAt5PrimeEnd = true;
			}
			else // read maps to plus strand
			{
				// get a pointer to counter of the position of the read
				position = &(readCounts[STRAND_PLUS][record.rID][record.beginPos]);

				// check if base at 5' end is uridine
				size_t clippedBasesAt5PrimeEnd = 0;
				if (record.cigar[0].operation == 'S')
					clippedBasesAt5PrimeEnd = record.cigar[0].count;
				if ((record.seq[clippedBasesAt5PrimeEnd] == 'T') || (record.seq[clippedBasesAt5PrimeEnd] == 't'))
					position->UAt5PrimeEnd = true;
			}

			// increase stack height
			position->reads += readWeight;
		}
	}

	return 0;
}

/*{
	map< unsigned int, double> means;
	map< unsigned int, double> stddevs;

	// calculate mean for every bin
	for (int overlap = MIN_ARBITRARY_OVERLAP; overlap <= MAX_ARBITRARY_OVERLAP; overlap++)
		if (overlap != PING_PONG_OVERLAP) // ignore ping-pong stacks, since they would skew the result
			if (overlap != testOverlap) // skip stacks with the overlap that we are testing
				for (unsigned int bin = 0; bin < bins; bin++)
					means[bin] += scoreHistogramsByOverlap[overlap][bin];
	for (unsigned int bin = 0; bin < bins; bin++)
		means[bin] /= MAX_ARBITRARY_OVERLAP - MIN_ARBITRARY_OVERLAP + 1;

	// calculate standard deviation for every bin
	for (int overlap = MIN_ARBITRARY_OVERLAP; overlap <= MAX_ARBITRARY_OVERLAP; overlap++)
		if (overlap != PING_PONG_OVERLAP) // ignore ping-pong stacks, since they would skew the result
			if (overlap != testOverlap) // skip stacks with the overlap that we are testing
				for (unsigned int bin = 0; bin < bins; bin++)
					stddevs[bin] += pow(scoreHistogramsByOverlap[overlap][bin] - means[bin], 2);
	for (unsigned int bin = 0; bin < bins; bin++)
		stddevs[bin] = sqrt(1 / (MAX_ARBITRARY_OVERLAP - MIN_ARBITRARY_OVERLAP) * stddevs[bin]);

	// calculate score for each bin, based on divergence from mean and variance
	for (unsigned int bin = 0; bin < bins; bin++)
	{
		double mean = means[bin];
		double stddev = stddevs[bin];
			unsigned int value = scoreHistogramsByOverlap[testOverlap][bin];
		scoresByBin[bin] = statisticalSignificance(mean, stddev, value) * value / (value + mean);
	}
}*/

// convert stack heights to scores
// todo: document parameters
void mapHeightsToScores(TCountsGenome &readCounts, THeightScoreMap &heightScoreMap)
{
	// iterate through all strands, contigs and positions to count how many stacks there are of any given height
	for (unsigned int strand = STRAND_PLUS; strand <= STRAND_MINUS; ++strand)
		for (TCountsStrand::iterator contig = readCounts[strand].begin(); contig != readCounts[strand].end(); ++contig)
			for (TCountsContig::iterator position = contig->second.begin(); position != contig->second.end(); ++position)
				heightScoreMap[position->second.reads + 0.5] += 1;
	cout << heightScoreMap.size();
}

// todo: description
void countStacksByGroup(TCountsGenome &readCounts, THeightScoreMap &heightScoreMap, TGroupedStackCountsByOverlap &groupedStackCountsByOverlap)
{
	// the following loop initializes a multi-dimensional array of stack counts with the following boundaries:
	// MAX_ARBITRARY_OVERLAP - MIN_ARBITRARY_OVERLAP + 1 (one for each possible overlap)
	// HEIGHT_SCORE_BINS (one of each bin of the height scores)
	// 2 (one for reads with uridine at the 5' end of reads on the + strand and one for those with a different base)
	// 2 (one for reads with uridine at the 5' end of reads on the - strand and one for those with a different base)
	// 2 (one for stack heights below the local coverage and one for stack heights above)
	groupedStackCountsByOverlap.resize(MAX_ARBITRARY_OVERLAP - MIN_ARBITRARY_OVERLAP + 1);
	for (TGroupedStackCountsByOverlap::iterator i = groupedStackCountsByOverlap.begin(); i != groupedStackCountsByOverlap.end(); ++i)
	{
		i->resize(HEIGHT_SCORE_BINS);
		for (TGroupedStackCounts::iterator j = i->begin(); j != i->end(); ++j)
		{
			j->resize(2);
			for (vector< vector< vector< float > > >::iterator k = j->begin(); k != j->end(); ++k)
			{
				k->resize(2);
				for (vector< vector< float > >::iterator l = k->begin(); l != k->end(); ++l)
					l->resize(2);
			}
		}
	}

	// the highest possible score is that of two overlapping stacks with the smallest height
	const float maxHeightScore = log10(heightScoreMap.begin()->second * heightScoreMap.begin()->second);

	// iterate through all strands, contigs and positions to find those positions where a stack on the plus strand overlaps a stack on the minus strand by <overlap> nt
	for (TCountsStrand::iterator contigPlusStrand = readCounts[STRAND_PLUS].begin(); contigPlusStrand != readCounts[STRAND_PLUS].end(); ++contigPlusStrand)
	{
		TCountsStrand::iterator contigMinusStrand = readCounts[STRAND_MINUS].find(contigPlusStrand->first);
		if (contigMinusStrand != readCounts[STRAND_MINUS].end())
		{
			for (TCountsContig::iterator positionPlusStrand = contigPlusStrand->second.begin(); positionPlusStrand != contigPlusStrand->second.end(); ++positionPlusStrand)
			{
				vector< TCountsPosition* > stacksOnMinusStrand(MAX_ARBITRARY_OVERLAP - MIN_ARBITRARY_OVERLAP + 1);
				float meanStackHeightInVicinity = 0;
				float maxStackHeightInVicinity = 0;
				for (int overlap = MIN_ARBITRARY_OVERLAP; overlap <= MAX_ARBITRARY_OVERLAP; overlap++)
				{
					TCountsContig::iterator positionMinusStrand = contigMinusStrand->second.find(positionPlusStrand->first + overlap);
					if (positionMinusStrand != contigMinusStrand->second.end())
					{
						// calculate mean of stack heights in the vicinity
						meanStackHeightInVicinity += positionMinusStrand->second.reads;
						// find highest stacks height in the vicinity
						if (positionMinusStrand->second.reads > maxStackHeightInVicinity)
							maxStackHeightInVicinity = positionMinusStrand->second.reads;
						// remember the stacks that we found, so we do not have to search them again
						stacksOnMinusStrand[overlap + MIN_ARBITRARY_OVERLAP] = &(positionMinusStrand->second);
					}
				}
				meanStackHeightInVicinity /= stacksOnMinusStrand.size();

				if (maxStackHeightInVicinity > 0) // only continue, if there are any stacks in the vicinity at all
				{
					float heightScorePlus = heightScoreMap[positionPlusStrand->second.reads];

					for (int overlap = MIN_ARBITRARY_OVERLAP; overlap <= MAX_ARBITRARY_OVERLAP; overlap++)
					{
						if (stacksOnMinusStrand[overlap + MIN_ARBITRARY_OVERLAP])
						{
							// calculate score based on heights of overlapping stacks
							float heightScore = heightScorePlus * heightScoreMap[stacksOnMinusStrand[overlap + MIN_ARBITRARY_OVERLAP]->reads];
							// find the bin for the score

							unsigned int heightScoreBin =
								static_cast<int>(0.5 // add 0.5 for arithmetic rounding when casting float to int
								+ log10(heightScore) // take logarithm of score
								/ maxHeightScore * (groupedStackCountsByOverlap[overlap + MIN_ARBITRARY_OVERLAP].size() - 1)); // assign every score to a bin

							// calculate score based on how much higher the stack is compared to the stacks in the vicinity
							float localHeightScore = (stacksOnMinusStrand[overlap + MIN_ARBITRARY_OVERLAP]->reads - (meanStackHeightInVicinity - stacksOnMinusStrand[overlap + MIN_ARBITRARY_OVERLAP]->reads/stacksOnMinusStrand.size())) / maxStackHeightInVicinity;
							// 0.2 seems to be the magical threshold that best segregates ping-pong overlaps from arbitrary overlaps
							unsigned int localHeightScoreBin = (localHeightScore < 0.2) ? IS_BELOW_COVERAGE : IS_ABOVE_COVERAGE;

							if (overlap == PING_PONG_OVERLAP)
							{
								// calculate score based on whether the stack on the + strand has Uridine at the 5' end
								unsigned int uridinePlusBin = (positionPlusStrand->second.UAt5PrimeEnd) ? IS_URIDINE : IS_NOT_URIDINE;
								// calculate score based on whether the stack on the - strand has Uridine at the 5' end
								unsigned int uridineMinusBin = (stacksOnMinusStrand[overlap + MIN_ARBITRARY_OVERLAP]->UAt5PrimeEnd) ? IS_URIDINE : IS_NOT_URIDINE;
								// increase bin counter
								groupedStackCountsByOverlap[overlap + MIN_ARBITRARY_OVERLAP][heightScoreBin][uridinePlusBin][uridineMinusBin][localHeightScoreBin]++;
							}
							else
							{
								// We assume a fixed probability of 25% of having uridine at the 5' end of reads.
								// Therefore, we add fractions to all bin counters.
								groupedStackCountsByOverlap[overlap + MIN_ARBITRARY_OVERLAP][heightScoreBin][IS_URIDINE][IS_URIDINE][localHeightScoreBin] += URIDINE_PROBABILITY * URIDINE_PROBABILITY;
								groupedStackCountsByOverlap[overlap + MIN_ARBITRARY_OVERLAP][heightScoreBin][IS_NOT_URIDINE][IS_URIDINE][localHeightScoreBin] += (1-URIDINE_PROBABILITY) * URIDINE_PROBABILITY;
								groupedStackCountsByOverlap[overlap + MIN_ARBITRARY_OVERLAP][heightScoreBin][IS_URIDINE][IS_NOT_URIDINE][localHeightScoreBin] += URIDINE_PROBABILITY * (1-URIDINE_PROBABILITY);
								groupedStackCountsByOverlap[overlap + MIN_ARBITRARY_OVERLAP][heightScoreBin][IS_NOT_URIDINE][IS_NOT_URIDINE][localHeightScoreBin] += (1-URIDINE_PROBABILITY) * (1-URIDINE_PROBABILITY);
							}
						}
					}
				}
			}
		}
	}
}

//todo: description
void collapseBins(TGroupedStackCountsByOverlap &groupedStackCountsByOverlap)
{
	// create a new container to hold the collapsed bin counts
	TGroupedStackCountsByOverlap collapsed = groupedStackCountsByOverlap;

	unsigned int collapsedBin = 0;
	unsigned int bin = 0;
	while (bin < groupedStackCountsByOverlap[0].size()){

		// initialize collapsed bin with 0
		for (TGroupedStackCountsByOverlap::iterator i = collapsed.begin(); i != collapsed.end(); ++i)
			for (vector< vector< vector< float > > >::iterator j = (*i)[collapsedBin].begin(); j != (*i)[collapsedBin].end(); ++j)
				for (vector< vector< float > >::iterator k = j->begin(); k != j->end(); ++k)
					for (vector< float >::iterator l = k->begin(); l != k->end(); ++l)
						*l = 0;

		unsigned int emptyBins;
		do {
			emptyBins = 0;

			// collapse bins
			for (unsigned int overlap = 0; overlap < groupedStackCountsByOverlap.size(); overlap++)
				for (unsigned int i = 0; i < groupedStackCountsByOverlap[overlap][bin].size(); i++)
					for (unsigned int j = 0; j < groupedStackCountsByOverlap[overlap][bin][i].size(); j++)
						for (unsigned int k = 0; k < groupedStackCountsByOverlap[overlap][bin][i][j].size(); ++k)
						{
							collapsed[overlap][collapsedBin][i][j][k] += groupedStackCountsByOverlap[overlap][bin][i][j][k];
							// check if collapsedBin is still empty to decide whether to collapse even more
							if (overlap != PING_PONG_OVERLAP)
								if (collapsed[overlap][collapsedBin][i][j][k] <= 0)
									emptyBins++;
						}

			bin++;
		// stop collapsing bins, when there are no empty bins or when all bins have been merged
		} while ((emptyBins > 0) && (bin < groupedStackCountsByOverlap[0].size()));

		collapsedBin++;
	}

	// shrink container to new number of bins
	for (TGroupedStackCountsByOverlap::iterator i = collapsed.begin(); i != collapsed.end(); ++i)
		i->resize(collapsedBin);

/*cout << "collapsedBins" << collapsed[0].size();
for (unsigned int overlap = 0; overlap < collapsed.size(); overlap++) {
			cout << "overlap" << overlap << endl;
			for (unsigned int bin = 0; bin < collapsed[overlap].size(); bin++) {
				cout << "	bin" << bin << endl;
				for (unsigned int i = 0; i < collapsed[overlap][bin].size(); i++) {
					cout << "		uplus" << i << endl;
					for (unsigned int j = 0; j < collapsed[overlap][bin][i].size(); j++) {
						cout << "			uminus" << j << endl;
						for (unsigned int k = 0; k < collapsed[overlap][bin][i][j].size(); ++k) {
							cout << "				localheight" << k << endl;
							cout << "					" << collapsed[overlap][bin][i][j][k] << endl;
						}
					}
				}
			}
		}
		cout.flush();

		for (unsigned int bin = 0; bin < collapsed[0].size(); bin++) {
			for (unsigned int i = 0; i < collapsed[0][bin].size(); i++) {
				for (unsigned int j = 0; j < collapsed[0][bin][i].size(); j++) {
					for (unsigned int k = 0; k < collapsed[0][bin][i][j].size(); ++k) {
						for (unsigned int overlap = 0; overlap < collapsed.size(); overlap++) {
							cout << collapsed[overlap][bin][i][j][k];
							if (overlap < collapsed.size() -1)
								cout << " ";
						}
						cout << endl;
					}
				}
			}
		}
		cout.flush();
*/
	// return collapsed bins as result
	groupedStackCountsByOverlap = collapsed;
}

/*
// find those positions on the genome where reads on opposite strands overlap by 10 nucleotides
// todo: document parameters
int findOverlappingReads(ofstream &bedGraphFile, TCountsGenome &readCounts, const TNameStore &bamNameStore, unsigned int minStackHeight, THeightScoreMap &heightScoreMap, TCombinedStackScoreMap &combinedStackScoreMap)
{
	// find those positions where reads on both strands overlap by <overlap> nucleotides
	for (int overlap = MIN_ARBITRARY_OVERLAP; overlap <= MAX_ARBITRARY_OVERLAP; overlap++)
	{
		if (overlap == PING_PONG_OVERLAP)
			continue;

		// iterate through all contigs on the plus strand
		for (TCountsStrand::iterator contigPlusStrand = readCounts[STRAND_PLUS].begin(); contigPlusStrand != readCounts[STRAND_PLUS].end(); ++contigPlusStrand)
		{
			// check if there are any stacks for the given contig on the minus strand
			TCountsStrand::iterator contigMinusStrand = readCounts[STRAND_MINUS].find(contigPlusStrand->first);
			if (contigMinusStrand != readCounts[STRAND_MINUS].end())
			{
				// iterate through all stacks on the plus strand
				for (TCountsContig::iterator positionPlusStrand = contigPlusStrand->second.begin(); positionPlusStrand != contigPlusStrand->second.end(); ++positionPlusStrand)
				{
					// check if there is a stack on the minus strand which overlaps with the stack on the plus strand by <overlap> nucleotides
					TCountsContig::iterator positionMinusStrand = contigMinusStrand->second.find(positionPlusStrand->first + overlap);
					if (positionMinusStrand != contigMinusStrand->second.end())
					{
					}
				}
			}
		}
	}

	return 0;
}*/

// todo: description
void plotHistograms(TGroupedStackCountsByOverlap &groupedStackCountsByOverlap, unsigned int dimension, const string &title, vector< string > xAxisLabels, bool logScale = false)
{
	// determine size of given dimension and give histogram plot as many bars
	unsigned int histogramBars = 0;
	switch (dimension)
	{
		case 0:
			histogramBars = groupedStackCountsByOverlap[0].size();
			break;
		case 1:
			histogramBars = groupedStackCountsByOverlap[0][0].size();
			break;
		case 2:
			histogramBars = groupedStackCountsByOverlap[0][0][0].size();
			break;
		case 3:
			histogramBars = groupedStackCountsByOverlap[0][0][0][0].size();
			break;
	}
	vector< vector< float > > histograms(groupedStackCountsByOverlap.size(), vector< float >(histogramBars, 0));

	// sum up bins grouped by the given dimension
	for (unsigned int overlap = 0; overlap < groupedStackCountsByOverlap.size(); overlap++)
		for (unsigned int i = 0; i < groupedStackCountsByOverlap[overlap].size(); i++)
			for (unsigned int j = 0; j < groupedStackCountsByOverlap[overlap][i].size(); j++)
				for (unsigned int k = 0; k < groupedStackCountsByOverlap[overlap][i][j].size(); ++k)
					for (unsigned int l = 0; l < groupedStackCountsByOverlap[overlap][i][j][k].size(); ++l)
						switch (dimension)
						{
							case 0:
								histograms[overlap][i] += groupedStackCountsByOverlap[overlap][i][j][k][l];
								break;
							case 1:
								histograms[overlap][j] += groupedStackCountsByOverlap[overlap][i][j][k][l];
								break;
							case 2:
								histograms[overlap][k] += groupedStackCountsByOverlap[overlap][i][j][k][l];
								break;
							case 3:
								histograms[overlap][l] += groupedStackCountsByOverlap[overlap][i][j][k][l];
								break;
						}

	// generate an R script that produces a histogram plot
	string fileName = title;
	replace(fileName.begin(), fileName.end(), ' ', '_'); // replace blanks in file name
	ofstream rScript(toCString(fileName + ".R"));

	rScript << "histograms <- data.frame("; // store histogram counts in a data frame
	for (int overlap = MIN_ARBITRARY_OVERLAP; overlap <= MAX_ARBITRARY_OVERLAP; overlap++)
	{
			// print height of bars
			rScript << endl << "overlap_";
			if (overlap < 0)
				rScript << "minus_";
			rScript << abs(overlap) << "=c(";
			for (unsigned int bar = 0; bar < histograms[overlap-MIN_ARBITRARY_OVERLAP].size(); bar++)
			{
				if (bar % 10 == 0)
					rScript << endl; // insert a line-break every once in a while, because R cannot parse very long lines
				rScript << histograms[overlap-MIN_ARBITRARY_OVERLAP][bar];
				if (bar < histograms[overlap-MIN_ARBITRARY_OVERLAP].size() - 1)
					rScript << ", "; // separate values by comma, unless it is the last one
			}
			rScript << ")" << endl; // close column of data frame

			if (overlap < MAX_ARBITRARY_OVERLAP)
				rScript << ", "; // separate columns of data frame by comma, unless it is the last column
	}
	rScript << ")" << endl; // close data frame


	// save plot as PNG
	rScript
		<< "options(bitmapType='cairo')" << endl
		<< "png('" << fileName << ".png')" << endl;

	// wrap histogram counts in "log10()", if y-axis should be log-scaled
	if (logScale)
		rScript << "histograms <- log10(histograms)" << endl;

	rScript	<< "plot(0, 0, xlim=c(0," << histograms[0].size() << "), type='n', xlab='" << title << "'";
	
	if (logScale)
	{
		rScript << ", ylim=c(0,max(histograms,0)), ylab='log10(frequency)'";
	}
	else
	{
		rScript << ", ylim=c(0,max(histograms)), ylab='frequency'";
	}

	rScript << ", xaxt='n')" << endl;

	// draw x-axis
	if (xAxisLabels.size() == 0)
	{
		// auto-generate x-axis based on quantiles, if no custom x-axis labels are given
		rScript << "axis(1, at=quantile(c(0," << histograms[0].size() << "), probs = seq(0, 1, 0.2))+0.5, labels=quantile(c(0," << histograms[0].size() << "), probs = seq(0, 1, 0.2)))" << endl;
	}
	else
	{
		// use custom x-axis labels to generate x-axis
		rScript << "axis(1, at=0:" << (xAxisLabels.size() - 1) << "+0.5, labels=c(";
		for (unsigned int i = 0; i < xAxisLabels.size() - 1; i++)
			rScript << "'" << xAxisLabels[i] << "', ";
		rScript << "'" << xAxisLabels[xAxisLabels.size() - 1] << "'))" << endl;
	}

	rScript
		// draw bars for arbitrary overlaps
		<< "for (overlap in " << MIN_ARBITRARY_OVERLAP << ":" << MAX_ARBITRARY_OVERLAP << ")" << endl
		<< "	if (overlap != 10)" << endl
		<< "		barplot(histograms[,gsub('-', 'minus_', paste('overlap_', overlap, sep=''))], col=rgb(0,0,0,alpha=0.1), border=NA, axes=FALSE, add=TRUE, width=1, space=0)" << endl
		// draw a red line for ping-pong overlaps
		<< "for (bin in 1:" << histograms[0].size() << ")" << endl
		<< "	lines(c(bin-1, bin), c(histograms[bin, 'overlap_10'], histograms[bin, 'overlap_10']), type='l', col='red', lwd=2)" << endl
		// draw legend
		<< "legend(x='top', c('10 nt overlap', 'arbitrary overlaps'), col=c('red', 'black'), ncol=2, lwd=c(3,3), xpd=TRUE, inset=-0.1)" << endl
		<< "garbage <- dev.off()" << endl;

	// close R script
	rScript.close();

	// execute R script with "Rscript"
	string RCommand = "Rscript '" + fileName + ".R'";
	system(toCString(RCommand));
}
void plotHistograms(TGroupedStackCountsByOverlap &groupedStackCountsByOverlap, unsigned int dimension, const string &title, bool logScale = false)
{
	vector< string > xAxisLabels;
	plotHistograms(groupedStackCountsByOverlap, dimension, title, xAxisLabels, logScale);
}

// program entry point
int main(int argc, char const ** argv)
{
	// parse the command line options
	AppOptions options;
	if (parseCommandLine(options, argc, argv) != ArgumentParser::PARSE_OK)
		return 1;

	TCountsGenome readCounts; // stats about positions where reads on the minus strand overlap with the 5' ends of reads on the plus strand

	TNameStore bamNameStore; // structure to store contig names

	// read all BAM/SAM files
	if (options.verbosity >= 3)
		cerr << "Counting reads in SAM/BAM files" << endl;
	for(TInputFiles::iterator inputFile = options.inputFiles.begin(); inputFile != options.inputFiles.end(); ++inputFile)
	{
		stopwatch(toCString(*inputFile), options.verbosity);

		// open SAM/BAM file
		BamStream bamFile(toCString(*inputFile));
		if (!isGood(bamFile))
		{
			cerr << "Failed to open input file: " << *inputFile << endl;
			return 1;
		}

		// for every position in the genome, count the number of reads that start at a given position
		if (countReadsInBamFile(bamFile, readCounts, options.minReadLength, options.maxReadLength, options.countMultiHits) != 0)
			return 1;

		// remember @SQ header lines from BAM file for mapping of contig IDs to human-readable names
		if (length(bamNameStore) == 0)
		{
			bamNameStore = nameStore(bamFile.bamIOContext);
		}
		else
		{
			// if multiple BAM files are given, check if headers are identical
			TNameStore tempBamNameStore = nameStore(bamFile.bamIOContext);
			bool nameStoresDiffer = false;
			TNameStoreIterator bamNameStoreIterator = begin(bamNameStore);
			TNameStoreIterator tempBamNameStoreIterator = begin(tempBamNameStore);
			while ((bamNameStoreIterator != end(bamNameStore)) && (tempBamNameStoreIterator != end(tempBamNameStore)) && !nameStoresDiffer)
			{
				if (value(bamNameStoreIterator) != value(tempBamNameStoreIterator))
					nameStoresDiffer = true;
				++bamNameStoreIterator;
				++tempBamNameStoreIterator;
			}
			if (nameStoresDiffer || (length(bamNameStore) != length(tempBamNameStore)))
			{
				cerr << "@SQ header lines of '" << *inputFile << "' differ from those of previous input files" << endl;
				return 1;
			}
		}

		// close SAM/BAM file
		close(bamFile);

		stopwatch(options.verbosity);
	}

	// go to output directory
	if (length(options.output) > 0)
	{
		mkdir(toCString(options.output), 0777);
		if (chdir(toCString(options.output)) != 0)
		{
			cerr << "Failed to open output directory: " << options.output;
			return 1;
		}
	}

	stopwatch("Binning stacks", options.verbosity);
	THeightScoreMap heightScoreMap;
	mapHeightsToScores(readCounts, heightScoreMap);
	TGroupedStackCountsByOverlap groupedStackCountsByOverlap;
	countStacksByGroup(readCounts, heightScoreMap, groupedStackCountsByOverlap);
	stopwatch(options.verbosity);

	stopwatch("Collapsing bins", options.verbosity);
	collapseBins(groupedStackCountsByOverlap);
	stopwatch(options.verbosity);

	if (options.plot)
	{
		stopwatch("Generating R plots", options.verbosity);
		plotHistograms(groupedStackCountsByOverlap, 0, "height score", true);
		vector< string > xAxisLabels;
		xAxisLabels.push_back("uridine"); xAxisLabels.push_back("not uridine");
		plotHistograms(groupedStackCountsByOverlap, 1, "base content at 5-prime end on forward strand", xAxisLabels);
		xAxisLabels.resize(0);
		xAxisLabels.push_back("uridine"); xAxisLabels.push_back("not uridine");
		plotHistograms(groupedStackCountsByOverlap, 2, "base content at 5-prime end on reverse strand", xAxisLabels);
		xAxisLabels.resize(0);
		xAxisLabels.push_back("average"); xAxisLabels.push_back("above average");
		plotHistograms(groupedStackCountsByOverlap, 3, "local height score", xAxisLabels);
		stopwatch(options.verbosity);
	}

/*	stopwatch("Scanning for overlapping reads", options.verbosity);
	ofstream bedGraphFile(toCString(options.outputBedGraph));
	if (bedGraphFile.fail())
	{
		cerr << "Failed to open bedGraph file: " << toCString(options.outputBedGraph) << endl;
		return 1;
	}
	// find positions where reads on the minus strand overlap with the 5' ends of reads on the plus strand
	if (findOverlappingReads(bedGraphFile, readCounts, bamNameStore, options.minStackHeight, heightScoreMap, combinedStackScoreMap) != 0)
		return 1;
	bedGraphFile.close();
	stopwatch(options.verbosity);
*/
	return 0;
}



