/*
 * Copyright Emweb BVBA, 3020 Herent, Belgium
 *
 * See LICENSE.txt for terms of use.
 */

#include "GlobalAligner.h"
#include "LocalAligner.h"
#include "SimpleScorer.h"
#include "GenomeScorer.h"
#include "Genbank.h"
#include "../args/args.hxx"

#include <fstream>
#include <ctime>

void removeGaps(seq::NTSequence& s)
{
  for (int i = 0; i < s.size(); ++i) {
    if (s[i] == seq::Nucleotide::GAP ||
	s[i] == seq::Nucleotide::MISSING) {
      s.erase(s.begin() + i);
      --i;
    }
  }
}

template<typename Aligner>
void runAga(Aligner& aligner, const Genome& ref, const std::string& queriesFile,
	    const std::vector<CdsFeature>& proteins,
	    const std::string& ntAlignmentFile,
	    const std::string& cdsAlignmentsFile,
	    const std::string& proteinAlignemntsFile,
	    const std::string& cdsNtAlignmentsFile,
	    const std::string& proteinNtAlignemntsFile)
{
  std::ifstream q(queriesFile);

  for (;;) {
    seq::NTSequence query;
    q >> query;

    query.sampleAmbiguities();
    removeGaps(query);

    if (!q)
      return;

    std::cout << "Started alignment of " << query.name()
	      << " (len="
	      << query.size() << ") against "
	      << ref.name() << " (len=" << ref.size() << ")" << std::endl;

    typename Aligner::Solution solution;

    if (query.size() > 0)
      solution = aligner.align(ref, NTSequence6AA(query), 0);
    else {
      solution.score = 0;
      solution.cigar.push_back(CigarItem(CigarItem::RefSkipped, ref.size()));
    }

    std::cout << "Aligned: " /* << solution.score / (double)ref.scoreFactor()
				<< ": " */ << solution.cigar << std::endl;

    saveSolution(solution.cigar, ref, query, ntAlignmentFile);

    /*
     * Everything below here just provides the amino acid alignments
     * and statistics
     */
    auto ntStats = calcStats(ref, query, solution.cigar,
			     aligner.scorer().nucleotideScorer());

    std::cout << std::endl << "NT alignment: " << ntStats << std::endl;

    {
      std::vector<CDSAlignment> aaAlignments
	= getCDSAlignments(ref, ref.cdsFeatures(), query, solution.cigar, true);

      std::ofstream aa;
      if (!cdsAlignmentsFile.empty())
	aa.open(cdsAlignmentsFile);

      std::ofstream nt;
      if (!cdsNtAlignmentsFile.empty())
	nt.open(cdsNtAlignmentsFile);

      int aaScore = 0;

      std::cout << std::endl << "CDS alignments:" << std::endl;
      for (const auto& a : aaAlignments) {
	if (aa.is_open())
	  aa << a.ref.aaSequence() << a.query.aaSequence();
	if (nt.is_open())
	  nt << a.ref.ntSequence() << a.query.ntSequence();
	auto aaStats = calcStats(a.ref.aaSequence(), a.query.aaSequence(),
				 aligner.scorer().aminoAcidScorer(),
				 a.refFrameshifts.size() + a.queryFrameshifts);

	aaScore += aaStats.score;
	if (aaStats.coverage > 0)
	  std::cout << " AA " << a.ref.aaSequence().name()
		    << ": " << aaStats << std::endl;
      }

      std::cout << std::endl
		<< "Alignment score: " << ntStats.score << " (NT) + "
		<< aaScore << " (AA) = "
		<< ntStats.score + aaScore << std::endl;
    }

    if (!proteins.empty()) {
      std::vector<CDSAlignment> aaAlignments
	= getCDSAlignments(ref, proteins, query, solution.cigar, true);

      std::ofstream aa;
      if (!proteinAlignemntsFile.empty())
	aa.open(proteinAlignemntsFile);

      std::ofstream nt;
      if (!proteinNtAlignemntsFile.empty())
	nt.open(proteinNtAlignemntsFile);

      std::cout << std::endl << "Protein Product alignments:" << std::endl;
      for (const auto& a : aaAlignments) {
	if (aa.is_open())
	  aa << a.ref.aaSequence() << a.query.aaSequence();
	if (nt.is_open())
	  nt << a.ref.ntSequence() << a.query.ntSequence();

	auto aaStats = calcStats(a.ref.aaSequence(), a.query.aaSequence(),
				 aligner.scorer().aminoAcidScorer(),
				 a.refFrameshifts.size() + a.queryFrameshifts);
	if (aaStats.coverage > 0)
	  std::cout << " AA " << a.ref.aaSequence().name()
		    << ": " << aaStats << std::endl;
      }
    }

    break;
  }
}

static const int** ntScoreMatrix(int M, int E)
{
  const int *rowA = new int[4]{M,E,E,E};
  const int *rowC = new int[4]{E,M,E,E};
  const int *rowG = new int[4]{E,E,M,E};
  const int *rowT = new int[4]{E,E,E,M};

  const int **matrix = new const int *[4]{rowA, rowC, rowG, rowT};

  return matrix;
}

GenbankRecord readGenomeGb(const std::string& name)
{
  GenbankRecord result;

  std::ifstream f(name);
  f >> result;
  
  return result;
}

bool endsWith(const std::string& s, const std::string& ext)
{
  return s.length() >= ext.length() && s.substr(s.length() - ext.length()) == ext;
}

bool exists(const std::string& f)
{
  std::ifstream s(f.c_str());
  return s.good();
}

std::string file(const std::string& f, const std::string& ext)
{
  int dotPos = f.rfind('.');
  return f.substr(0, dotPos) + ext; 
}

void saveSolution(const Cigar& cigar,
		  const seq::NTSequence& ref, const seq::NTSequence& query,
		  const std::string& fname)
{
  seq::NTSequence seq1 = ref;
  seq::NTSequence seq2 = query;
  cigar.align(seq1, seq2);

  std::ofstream o(fname);
  o << seq1;
  o << seq2;
}

int main(int argc, char **argv)
{
  args::ArgumentParser parser
    ("This is AGA, a Genomic Aligner, (c) Emweb bvba\n"
     "See http://github.com/emweb/aga/LICENSE.txt for terms of use.",
     "AGA will compute the optimal pairwise alignment of a nucleic acid "
     "query sequence (QUERY.FASTA) against a reference genome (REFERENCE.GB), "
     "taking into account CDS annotations in the genbank record to include "
     "in the alignment score all amino acid alignments and minimizing "
     "frameshifts within these open reading frames. It writes the "
     "resulting alignment to ALIGNMENT.FASTA\n\n");
  args::HelpFlag help(parser, "help", "Display this help menu", {"help"});

  args::HelpFlag version(parser, "version", "Display the version", {"version"});

  args::Group group(parser, "Alignment mode, specify one of:",
		    args::Group::Validators::Xor);
  args::Flag global(group, "global", "Global alignment", {"global"});
  args::Flag local(group, "local", "Local alignment", {"local"});

  args::Group ntGroup(parser, "Nucleic Acid Score options",
		      args::Group::Validators::DontCare);
  args::ValueFlag<int> ntWeightFlag
    (ntGroup, "WEIGHT", "Weight for NT score fraction (default=1)",
     {"nt-weight"}, 1);
  args::ValueFlag<int> ntGapOpenFlag
    (ntGroup, "COST",
     "Nucleotide Gap Open penalty (default=-10)", {"nt-gap-open"}, -10);
  args::ValueFlag<int> ntGapExtendFlag
    (ntGroup, "COST", "Nucleotide Gap Extension penalty (default=-1)",
     {"nt-gap-extend"}, -1);
  args::ValueFlag<int> ntMatchFlag
    (ntGroup, "SCORE", "Score for a nucleotide match (default=2)",
     {"nt-match"}, 2);
  args::ValueFlag<int> ntMismMatchFlag
    (ntGroup, "COST", "Penalty for a nucleotide mismatch (default=-2)",
     {"nt-mismatch"}, -2);
  
  args::Group aaGroup(parser, "Amino Acid Score options",
		      args::Group::Validators::DontCare);
  args::ValueFlag<int> aaWeightFlag
    (aaGroup, "WEIGHT", "Total weight for AA score fraction (default=1)",
     {"aa-weight"}, 1);
  args::ValueFlag<int> aaGapOpenFlag
    (aaGroup, "COST", "Amino Acid Gap Open penalty (default=-6)",
     {"aa-gap-open"}, -6);
  args::ValueFlag<int> aaGapExtendFlag
    (aaGroup, "COST", "Amino Acid Gap Extension penalty (default=-2)",
     {"aa-gap-extend"}, -2);
  args::ValueFlag<std::string> aaMatrixFlag
    (aaGroup, "MATRIX",
     "Substitution matrix for amino acid matches: "
     "BLOSUM62 or BLOSUM30 (default=BLOSUM30)",
     {"aa-matrix"}, "BLOSUM30");
  args::ValueFlag<int> frameShiftPenaltyFlag
    (aaGroup, "COST",
     "Frameshift penalty (default=-100)",
     {"aa-frameshift"}, -100);
  args::ValueFlag<int> misaligntPenaltyFlag
    (aaGroup, "COST",
     "Codon misalignment penalty (default=-20)",
     {"aa-misalign"}, -20);

  args::Group aaOutputGroup(parser, "Amino acid alignments output",
			    args::Group::Validators::DontCare);
  args::ValueFlag<std::string> cdsOutput
    (aaOutputGroup, "ALIGNMENT.FASTA",
     "Amino acid alignments output file of CDS (FASTA)",
    {"cds-aa-alignments"});
  args::ValueFlag<std::string> cdsNtOutput
    (aaOutputGroup, "ALIGNMENT.FASTA",
     "Nucleic acid CDS alignments output file of CDS (FASTA)",
    {"cds-nt-alignments"});
  args::ValueFlag<std::string> proteinOutput
    (aaOutputGroup, "ALIGNMENT.FASTA",
     "Amino acid alignments output file of Protein Products (FASTA)",
    {"protein-aa-alignments"});
  args::ValueFlag<std::string> proteinNtOutput
    (aaOutputGroup, "ALIGNMENT.FASTA",
     "Nucleic acid CDS alignments output file of Protein Products (FASTA)",
    {"protein-nt-alignments"});

  args::Positional<std::string> genome
    (parser, "REFERENCE.GB", "Annotated reference (Genbank Record)");
  args::Positional<std::string> query
    (parser, "QUERY.FASTA", "FASTA file with nucleic acid query sequence");
  args::Positional<std::string> ntAlignment
    (parser, "ALIGNMENT.FASTA",
     "Nucleic acid alignment output file (FASTA)");

  try {
    parser.ParseCLI(argc, argv);
  } catch (args::Help e) {
    if (e.what() == help.Name()) {
      std::cerr << "Command-line help:" << std::endl << std::endl
		<< parser;
    } else if (e.what() == version.Name()) {
      std::cout << "AGA version 0.9" << std::endl;
    }
    return 0;
  } catch (args::ParseError e) {
    std::cerr << e.what() << std::endl << std::endl;
    std::cerr << "Command-line help:" << std::endl << std::endl
	      << parser;
    return 1;
  } catch (args::ValidationError e) {
    std::cerr << "Error: specify at least one of --global or --local"
	      << std::endl << std::endl;
    std::cerr << "Command-line help:" << std::endl << std::endl
	      << parser;
    return 1;
  }

  if (!genome || !query) {
    std::cerr << "Error: input files missing" << std::endl << std::endl;
    std::cerr << parser;
    return 1;
  }

  if (!ntAlignment) {
    std::cerr << "Error: output file missing" << std::endl << std::endl;
    std::cerr << parser;
    return 1;
  }
  
  std::string genomeFile = args::get(genome);
  std::string queriesFile = args::get(query);
  int ntWeight = args::get(ntWeightFlag);
  int aaWeight = args::get(aaWeightFlag);

  Genome ref;
  std::vector<CdsFeature> proteins;

  if (endsWith(genomeFile, ".fasta") && exists(file(genomeFile, ".cds")))
    ref = readGenome(genomeFile, file(genomeFile, ".cds"), proteins);
  else {
    GenbankRecord refGb = readGenomeGb(genomeFile);
    ref = getGenome(refGb);
    proteins = getProteins(ref, refGb);
  }
  
  std::cout << "Using CDS:" << std::endl;
  for (auto& f : ref.cdsFeatures()) {
    std::cout << " " << f.aaSeq.name() << " (len=" << f.aaSeq.size() << ")"
	      << std::endl;
  }
  std::cout << std::endl;

  const int **ntMat = ntScoreMatrix(args::get(ntMatchFlag),
				    args::get(ntMismMatchFlag));
  SimpleScorer<seq::NTSequence> ntScorer(ntMat,
					 args::get(ntGapOpenFlag),
					 args::get(ntGapExtendFlag),
					 0, 0);

  const int **aaMat;
  if (args::get(aaMatrixFlag) == "BLOSUM30")
    aaMat = SubstitutionMatrix::BLOSUM30();
  else if (args::get(aaMatrixFlag) == "BLOSUM62")
    aaMat = SubstitutionMatrix::BLOSUM62();
  else {
    std::cerr << "Error: --aa-matrix: illegal value" << std::endl << std::endl;
    std::cerr << parser;
    return 1;
  }    

  SimpleScorer<seq::AASequence> aaScorer(aaMat,
					 args::get(aaGapOpenFlag),
					 args::get(aaGapExtendFlag),
					 args::get(frameShiftPenaltyFlag),
					 args::get(misaligntPenaltyFlag));

#if 0
  std::cerr << " ";
  for (int j = 0; j < 26; ++j)
    std::cerr << "  " << seq::AminoAcid::fromRep(j);
  std::cerr << std::endl;

  for (int i = 0; i < 26; ++i) {
    std::cerr << seq::AminoAcid::fromRep(i);
    for (int j = 0; j < 26; ++j) {
      int f = aaMat[i][j];
      if (f < 0 || f > 10)
	std::cerr << " ";
      else
	std::cerr << "  ";
      std::cerr << aaMat[i][j];
    }
    std::cerr << std::endl;
  }
#endif

  ref.preprocess(ntWeight, aaWeight);
  GenomeScorer genomeScorer(ntScorer, aaScorer, ntWeight, aaWeight);

  if (local) {
    LocalAligner<GenomeScorer, Genome, NTSequence6AA, 3> aligner(genomeScorer);
    runAga(aligner, ref, queriesFile, proteins, args::get(ntAlignment),
	   args::get(cdsOutput), args::get(proteinOutput),
	   args::get(cdsNtOutput), args::get(proteinNtOutput));
  } else {
    GlobalAligner<GenomeScorer, Genome, NTSequence6AA, 3> aligner(genomeScorer);
    runAga(aligner, ref, queriesFile, proteins, args::get(ntAlignment),
	   args::get(cdsOutput), args::get(proteinOutput),
	   args::get(cdsNtOutput), args::get(proteinNtOutput));
  }

  return 0;
}
