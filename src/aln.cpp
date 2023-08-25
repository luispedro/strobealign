#include "aln.hpp"

#include <iostream>
#include <math.h>
#include <sstream>
#include "revcomp.hpp"
#include "timer.hpp"
#include "nam.hpp"
#include "paf.hpp"
#include "aligner.hpp"

using namespace klibpp;


static inline Alignment get_alignment(
    const Aligner& aligner,
    const Nam &nam,
    const References& references,
    const Read& read,
    bool consistent_nam
);

static inline bool score(const Nam &a, const Nam &b) {
    return a.score > b.score;
}

/*
 * Determine whether the NAM represents a match to the forward or
 * reverse-complemented sequence by checking in which orientation the
 * first and last strobe in the NAM match
 *
 * - If first and last strobe match in forward orientation, return true.
 * - If first and last strobe match in reverse orientation, update the NAM
 *   in place and return true.
 * - If first and last strobe do not match consistently, return false.
 */
bool reverse_nam_if_needed(Nam& nam, const Read& read, const References& references, int k) {
    auto read_len = read.size();
    std::string ref_start_kmer = references.sequences[nam.ref_id].substr(nam.ref_start, k);
    std::string ref_end_kmer = references.sequences[nam.ref_id].substr(nam.ref_end-k, k);

    std::string seq, seq_rc;
    if (nam.is_rc) {
        seq = read.rc;
        seq_rc = read.seq;
    } else {
        seq = read.seq;
        seq_rc = read.rc;
    }
    std::string read_start_kmer = seq.substr(nam.query_start, k);
    std::string read_end_kmer = seq.substr(nam.query_end-k, k);
    if (ref_start_kmer == read_start_kmer && ref_end_kmer == read_end_kmer) {
        return true;
    }

    // False forward or false reverse (possible due to symmetrical hash values)
    //    we need two extra checks for this - hopefully this will remove all the false hits we see (true hash collisions should be very few)
    int q_start_tmp = read_len - nam.query_end;
    int q_end_tmp = read_len - nam.query_start;
    // false reverse hit, change coordinates in nam to forward
    read_start_kmer = seq_rc.substr(q_start_tmp, k);
    read_end_kmer = seq_rc.substr(q_end_tmp - k, k);
    if (ref_start_kmer == read_start_kmer && ref_end_kmer == read_end_kmer) {
        nam.is_rc = !nam.is_rc;
        nam.query_start = q_start_tmp;
        nam.query_end = q_end_tmp;
        return true;
    }
    return false;
}

static inline void align_SE(
    const Aligner& aligner,
    Sam& sam,
    std::vector<Nam>& nams,
    const KSeq& record,
    int k,
    const References& references,
    Details& details,
    float dropoff_threshold,
    int max_tries,
    unsigned max_secondary
) {
    if (nams.empty()) {
        sam.add_unmapped(record);
        return;
    }

    Read read(record.seq);
    std::vector<Alignment> alignments;
    int tries = 0;
    Nam n_max = nams[0];

    int best_edit_distance = std::numeric_limits<int>::max();
    int best_score = -1000;

    Alignment best_alignment;
    best_alignment.score = -100000;
    best_alignment.is_unaligned = true;
    int min_mapq_diff = best_edit_distance;

    for (auto &nam : nams) {
        float score_dropoff = (float) nam.n_hits / n_max.n_hits;
        if (tries >= max_tries || (tries > 1 && best_edit_distance == 0) || score_dropoff < dropoff_threshold) {
            break;
        }
        bool consistent_nam = reverse_nam_if_needed(nam, read, references, k);
        details.nam_inconsistent += !consistent_nam;
        auto alignment = get_alignment(aligner, nam, references, read, consistent_nam);
        details.tried_alignment++;
        details.gapped += alignment.gapped;

        int diff_to_best = std::abs(best_score - alignment.score);
        min_mapq_diff = std::min(min_mapq_diff, diff_to_best);

        if (max_secondary > 0) {
            alignments.emplace_back(alignment);
        }
        if (alignment.score > best_score) {
            min_mapq_diff = std::max(0, alignment.score - best_score); // new distance to next best match
            best_score = alignment.score;
            best_alignment = std::move(alignment);
            if (max_secondary == 0) {
                best_edit_distance = best_alignment.global_ed;
            }
        }
        tries++;
    }
    if (max_secondary == 0) {
        best_alignment.mapq = std::min(min_mapq_diff, 60);
        sam.add(best_alignment, record, read.rc, true, details);
        return;
    }
    // Sort alignments by score, highest first
    std::sort(alignments.begin(), alignments.end(),
        [](const Alignment& a, const Alignment& b) -> bool {
            return a.score > b.score;
        }
    );

    auto max_out = std::min(alignments.size(), static_cast<size_t>(max_secondary) + 1);
    for (size_t i = 0; i < max_out; ++i) {
        auto alignment = alignments[i];
        if (alignment.score - best_score > 2*aligner.parameters.mismatch + aligner.parameters.gap_open) {
            break;
        }
        bool is_primary = i == 0;
        if (is_primary) {
            alignment.mapq = std::min(min_mapq_diff, 60);
        } else {
            alignment.mapq = 255;
        }
        sam.add(alignment, record, read.rc, is_primary, details);
    }
}

static inline Alignment align_segment(
    const Aligner& aligner,
    const std::string &read_segm,
    const std::string &ref_segm,
    int ref_start,
    int ext_left,
    int ext_right,
    bool consistent_nam,
    bool is_rc
) {
    Alignment alignment;
    auto read_segm_len = read_segm.size();
    // The ref_segm includes an extension of ext_left bases upstream and ext_right bases downstream
    auto ref_segm_len_ham = ref_segm.size() - ext_left - ext_right; // we send in the already extended ref segment to save time. This is not true in center alignment if merged match have diff length
    if (ref_segm_len_ham == read_segm_len && consistent_nam) {
        std::string ref_segm_ham = ref_segm.substr(ext_left, read_segm_len);

        auto hamming_dist = hamming_distance(read_segm, ref_segm_ham);

        if (hamming_dist >= 0 && (((float) hamming_dist / read_segm_len) < 0.05) ) { //Hamming distance worked fine, no need to ksw align
            auto info = hamming_align(read_segm, ref_segm_ham, aligner.parameters.match, aligner.parameters.mismatch, aligner.parameters.end_bonus);
            alignment.cigar = std::move(info.cigar);
            alignment.edit_distance = info.edit_distance;
            alignment.score = info.sw_score;
            alignment.ref_start = ref_start + ext_left + info.query_start;
            alignment.is_rc = is_rc;
            alignment.is_unaligned = false;
            alignment.length = read_segm_len;
            return alignment;
        }
    }
    auto info = aligner.align(read_segm, ref_segm);
    alignment.cigar = std::move(info.cigar);
    alignment.edit_distance = info.edit_distance;
    alignment.score = info.sw_score;
    alignment.ref_start = ref_start + info.ref_start;
    alignment.is_rc = is_rc;
    alignment.is_unaligned = false;
    alignment.length = info.ref_span();
    return alignment;
}


/*
 Extend a NAM so that it covers the entire read and return the resulting
 alignment.
*/
/*
 Only the following fields of the 'nam' struct are used:
 - is_rc (r/w)
 - ref_id (read)
 - ref_s (read)
 - ref_e (read)
 - query_s (r/w)
 - query_e (r/w)
 This is almost like a 'hit', except for ref_id.

 the nam is sent afterwards into
 - get_MAPQ, which only uses .score and .n_hits
 - rescue_mate, which ...?
*/

static inline Alignment get_alignment(
    const Aligner& aligner,
    const Nam &nam,
    const References& references,
    const Read& read,
    bool consistent_nam
) {
    const std::string query = nam.is_rc ? read.rc : read.seq;
    const std::string& ref = references.sequences[nam.ref_id];

    const auto projected_ref_start = std::max(0, nam.ref_start - nam.query_start);
    const auto projected_ref_end = std::min(nam.ref_end + query.size() - nam.query_end, ref.size());

    aln_info info;
    int result_ref_start;
    bool gapped = true;
    if (projected_ref_end - projected_ref_start == query.size() && consistent_nam) {
        std::string ref_segm_ham = ref.substr(projected_ref_start, query.size());
        auto hamming_dist = hamming_distance(query, ref_segm_ham);

        if (hamming_dist >= 0 && (((float) hamming_dist / query.size()) < 0.05) ) { //Hamming distance worked fine, no need to ksw align
            info = hamming_align(query, ref_segm_ham, aligner.parameters.match, aligner.parameters.mismatch, aligner.parameters.end_bonus);
            result_ref_start = projected_ref_start + info.ref_start;
            gapped = false;
        }
    }
    if (gapped) {
        const int diff = std::abs(nam.ref_span() - nam.query_span());
        const int ext_left = std::min(50, projected_ref_start);
        const int ref_start = projected_ref_start - ext_left;
        const int ext_right = std::min(std::size_t(50), ref.size() - nam.ref_end);
        const auto ref_segm_size = read.size() + diff + ext_left + ext_right;
        const auto ref_segm = ref.substr(ref_start, ref_segm_size);
        info = aligner.align(query, ref_segm);
        result_ref_start = ref_start + info.ref_start;
    }
    int softclipped = info.query_start + (query.size() - info.query_end);
    Alignment alignment;
    alignment.cigar = std::move(info.cigar);
    alignment.edit_distance = info.edit_distance;
    alignment.global_ed = info.edit_distance + softclipped;
    alignment.score = info.sw_score;
    alignment.ref_start = result_ref_start;
    alignment.length = info.ref_span();
    alignment.is_rc = nam.is_rc;
    alignment.is_unaligned = false;
    alignment.ref_id = nam.ref_id;
    alignment.gapped = gapped;

    return alignment;
}

static inline uint8_t get_mapq(const std::vector<Nam> &nams, const Nam &n_max) {
    if (nams.size() <= 1) {
        return 60;
    }
    const float s1 = n_max.score;
    const float s2 = nams[1].score;
    // from minimap2: MAPQ = 40(1−s2/s1) ·min{1,|M|/10} · log s1
    const float min_matches = std::min(n_max.n_hits / 10.0, 1.0);
    const int uncapped_mapq = 40 * (1 - s2 / s1) * min_matches * log(s1);
    return std::min(uncapped_mapq, 60);
}

static inline std::pair<int, int> joint_mapq_from_alignment_scores(float score1, float score2) {
    int mapq;
    if (score1 == score2) { // At least two identical placements
        mapq = 0;
    } else {
        const int diff = score1 - score2; // (1.0 - (S1 - S2) / S1);
//        float log10_p = diff > 6 ? -6.0 : -diff; // Corresponds to: p_error= 0.1^diff // change in sw score times rough illumina error rate. This is highly heauristic, but so seem most computations of mapq scores
        if (score1 > 0 && score2 > 0) {
            mapq = std::min(60, diff);
//            mapq1 = -10 * log10_p < 60 ? -10 * log10_p : 60;
        } else if (score1 > 0 && score2 <= 0) {
            mapq = 60;
        } else { // both negative SW one is better
            mapq = 1;
        }
    }
    return std::make_pair(mapq, mapq);
}

static inline float normal_pdf(float x, float m, float s)
{
    static const float inv_sqrt_2pi = 0.3989422804014327;
    const float a = (x - m) / s;

    return inv_sqrt_2pi / s * std::exp(-0.5f * a * a);
}

static inline bool score_sw(const Alignment &a, const Alignment &b)
{
    return a.score > b.score;
}

static inline bool sort_scores(const std::tuple<double, Alignment, Alignment> &a,
                               const std::tuple<double, Alignment, Alignment> &b)
{
    return std::get<0>(a) > std::get<0>(b);
}

static inline std::vector<std::tuple<double,Alignment,Alignment>> get_best_scoring_pairs(
    const std::vector<Alignment> &alignments1,
    const std::vector<Alignment> &alignments2,
    float mu,
    float sigma
) {
    std::vector<std::tuple<double,Alignment,Alignment>> pairs;
    for (auto &a1 : alignments1) {
        for (auto &a2 : alignments2) {
            float dist = std::abs(a1.ref_start - a2.ref_start);
            double score = a1.score + a2.score;
            if ((a1.is_rc ^ a2.is_rc) && (dist < mu + 4 * sigma)) {
                score += log(normal_pdf(dist, mu, sigma));
            }
            else { // individual score
                // 10 corresponds to a value of log(normal_pdf(dist, mu, sigma)) of more than 4 stddevs away
                score -= 10;
            }
            pairs.emplace_back(std::make_tuple(score, a1, a2));
        }
    }
    std::sort(pairs.begin(), pairs.end(), sort_scores); // Sorting by highest score first

    return pairs;
}

bool is_proper_nam_pair(const Nam nam1, const Nam nam2, float mu, float sigma) {
    if (nam1.ref_id != nam2.ref_id || nam1.is_rc == nam2.is_rc) {
        return false;
    }
    int a = std::max(0, nam1.ref_start - nam2.query_start);
    int b = std::max(0, nam2.ref_start - nam2.query_start);

    // r1 ---> <---- r2
    bool r1_r2 = nam2.is_rc && (a <= b) && (b - a < mu + 10*sigma);

     // r2 ---> <---- r1
    bool r2_r1 = nam1.is_rc && (b <= a) && (a - b < mu + 10*sigma);

    return r1_r2 || r2_r1;
}

static inline std::vector<std::tuple<int,Nam,Nam>> get_best_scoring_nam_locations(
    const std::vector<Nam> &nams1,
    const std::vector<Nam> &nams2,
    float mu,
    float sigma
) {
    std::vector<std::tuple<int,Nam,Nam>> joint_nam_scores;
    if (nams1.empty() && nams2.empty()) {
        return joint_nam_scores;
    }

    robin_hood::unordered_set<int> added_n1;
    robin_hood::unordered_set<int> added_n2;
    int joint_hits;
    int hjss = 0; // highest joint score seen
    for (auto &n1 : nams1) {
        for (auto &n2 : nams2) {
            if (n1.n_hits + n2.n_hits < hjss/2) {
                break;
            }
            if (is_proper_nam_pair(n1, n2, mu, sigma)) {
                joint_hits = n1.n_hits + n2.n_hits;
                joint_nam_scores.emplace_back(joint_hits, n1, n2);
                added_n1.insert(n1.nam_id);
                added_n2.insert(n2.nam_id);
                if (joint_hits > hjss) {
                    hjss = joint_hits;
                }
            }
        }
    }

    Nam dummy_nan;
    dummy_nan.ref_start = -1;
    if (!nams1.empty()) {
        int hjss1 = hjss > 0 ? hjss : nams1[0].n_hits;
        for (auto &n1 : nams1) {
            if (n1.n_hits  < hjss1/2){
                break;
            }
            if (added_n1.find(n1.nam_id) != added_n1.end()){
                continue;
            }
//            int diff1 = (n1.query_e - n1.query_s) - (n1.ref_e - n1.ref_s);
//            int  n1_penalty = diff1 > 0 ? diff1 : - diff1;
            joint_hits = n1.n_hits;
            std::tuple<int, Nam, Nam> t{joint_hits, n1, dummy_nan};
            joint_nam_scores.push_back(t);
        }
    }

    if ( !nams2.empty() ){
        int hjss2 = hjss  > 0 ? hjss : nams2[0].n_hits;
        //    int hjss2 = all_nams2[0].n_hits;
        for (auto &n2 : nams2) {
            if (n2.n_hits  < hjss2/2){
                break;
            }
            if (added_n2.find(n2.nam_id) != added_n2.end()){
                continue;
            }
//            int diff2 = (n2.query_e - n2.query_s) - (n2.ref_e - n2.ref_s);
//            int  n2_penalty = diff2 > 0 ? diff2 : - diff2;
            joint_hits = n2.n_hits;
            //                        std::cerr << S << " individual score " << x << " " << std::endl;
            std::tuple<int, Nam, Nam> t{joint_hits, dummy_nan, n2};
            joint_nam_scores.push_back(t);
        }
    }

    added_n1.clear();
    added_n2.clear();

    std::sort(
        joint_nam_scores.begin(),
        joint_nam_scores.end(),
        [](const std::tuple<int, Nam, Nam>& a, const std::tuple<int, Nam, Nam>& b) -> bool {
            return std::get<0>(a) > std::get<0>(b);
        }
    ); // Sort by highest score first

    return joint_nam_scores;
}

/*
 * Determine (roughly) whether the read sequence has some l-mer (with l = k*2/3)
 * in common with the reference sequence
 */
bool has_shared_substring(const std::string& read_seq, const std::string& ref_seq, int k) {
    int sub_size = 2 * k / 3;
    int step_size = k / 3;
    std::string submer;
    for (size_t i = 0; i + sub_size < read_seq.size(); i += step_size) {
        submer = read_seq.substr(i, sub_size);
        if (ref_seq.find(submer) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/* Return true iff rescue by alignment was actually attempted */
static inline bool rescue_mate(
    const Aligner& aligner,
    Nam &nam,
    const References& references,
    const Read& guide,
    const Read& read,
    Alignment &alignment,
    float mu,
    float sigma,
    int k
) {
    int a, b;
    std::string r_tmp;
    bool a_is_rc;
    auto read_len = read.size();

    reverse_nam_if_needed(nam, guide, references, k);
    if (nam.is_rc){
        r_tmp = read.seq;
        a = nam.ref_start - nam.query_start - (mu+5*sigma);
        b = nam.ref_start - nam.query_start + read_len/2; // at most half read overlap
        a_is_rc = false;
    } else {
        r_tmp = read.rc; // mate is rc since fr orientation
        a = nam.ref_end + (read_len - nam.query_end) - read_len/2; // at most half read overlap
        b = nam.ref_end + (read_len - nam.query_end) + (mu+5*sigma);
        a_is_rc = true;
    }

    auto ref_len = static_cast<int>(references.lengths[nam.ref_id]);
    auto ref_start = std::max(0, std::min(a, ref_len));
    auto ref_end = std::min(ref_len, std::max(0, b));

    if (ref_end < ref_start + k){
        alignment.cigar = Cigar();
        alignment.edit_distance = read_len;
        alignment.score = 0;
        alignment.ref_start =  0;
        alignment.is_rc = nam.is_rc;
        alignment.ref_id = nam.ref_id;
        alignment.is_unaligned = true;
//        std::cerr << "RESCUE: Caught Bug3! ref start: " << ref_start << " ref end: " << ref_end << " ref len:  " << ref_len << std::endl;
        return false;
    }
    std::string ref_segm = references.sequences[nam.ref_id].substr(ref_start, ref_end - ref_start);

    if (!has_shared_substring(r_tmp, ref_segm, k)){
        alignment.cigar = Cigar();
        alignment.edit_distance = read_len;
        alignment.score = 0;
        alignment.ref_start =  0;
        alignment.is_rc = nam.is_rc;
        alignment.ref_id = nam.ref_id;
        alignment.is_unaligned = true;
//        std::cerr << "Avoided!" << std::endl;
        return false;
//        std::cerr << "Aligning anyway at: " << ref_start << " to " << ref_end << "ref len:" << ref_len << " ref_id:" << n.ref_id << std::endl;
    }
    auto info = aligner.align(r_tmp, ref_segm);

//    if (info.ed == 100000){
//        std::cerr<< "________________________________________" << std::endl;
//        std::cerr<< "RESCUE MODE: " << mu << "  " << sigma << std::endl;
//        std::cerr<< read << "   " << read_rc << std::endl;
//        std::cerr << r_tmp << " " << n.n_hits << " " << n.score << " " << " " << alignment.ed << " "  <<  n.query_s << " "  << n.query_e << " "<<  n.ref_s << " "  << n.ref_e << " " << n.is_rc << " " << " " << alignment.cigar << " " << info.sw_score << std::endl;
//        std::cerr << "a " << a << " b " << b << " ref_start " <<  ref_start << " ref_end " << ref_end << "  ref_end - ref_start "  <<  ref_end - ref_start << "  n.is_flipped " <<  n.is_flipped << std::endl;
//        std::cerr<< "________________________________________" << std::endl;
//    }
//    info = parasail_align(ref_segm, ref_segm.size(), r_tmp, read_len, 1, 4, 6, 1);

//    ksw_extz_t ez;
//    const char *ref_ptr = ref_segm.c_str();
//    const char *read_ptr = r_tmp.c_str();
//    info = ksw_align(ref_ptr, ref_segm.size(), read_ptr, r_tmp.size(), 1, 4, 6, 1, ez);
//    std::cerr << "Cigar: " << info.cigar << std::endl;

    alignment.cigar = info.cigar;
    alignment.edit_distance = info.edit_distance;
    alignment.score = info.sw_score;
    alignment.ref_start = ref_start + info.ref_start;
    alignment.is_rc = a_is_rc;
    alignment.ref_id = nam.ref_id;
    alignment.is_unaligned = info.cigar.empty();
    alignment.length = info.ref_span();
    return true;
}


void rescue_read(
    const Read& read2,  // read to be rescued
    const Read& read1,  // read that has NAMs
    const Aligner& aligner,
    const References& references,
    std::vector<Nam> &nams1,
    int max_tries,
    float dropoff,
    std::array<Details, 2>& details,
    int k,
    float mu,
    float sigma,
    size_t max_secondary,
    double secondary_dropoff,
    Sam& sam,
    const klibpp::KSeq& record1,
    const klibpp::KSeq& record2,
    bool swap_r1r2  // TODO get rid of this
) {
    Nam n_max1 = nams1[0];
    int tries = 0;

    std::vector<Alignment> alignments1;
    std::vector<Alignment> alignments2;
    for (auto& nam : nams1) {
        float score_dropoff1 = (float) nam.n_hits / n_max1.n_hits;
        // only consider top hits (as minimap2 does) and break if below dropoff cutoff.
        if (tries >= max_tries || score_dropoff1 < dropoff) {
            break;
        }

        const bool consistent_nam = reverse_nam_if_needed(nam, read1, references, k);
        details[0].nam_inconsistent += !consistent_nam;
        auto alignment = get_alignment(aligner, nam, references, read1, consistent_nam);
        details[0].gapped += alignment.gapped;
        alignments1.emplace_back(alignment);
        details[0].tried_alignment++;

        // Force SW alignment to rescue mate
        Alignment a2;
        details[1].mate_rescue += rescue_mate(aligner, nam, references, read1, read2, a2, mu, sigma, k);
        alignments2.emplace_back(a2);

        tries++;
    }
    std::sort(alignments1.begin(), alignments1.end(), score_sw);
    std::sort(alignments2.begin(), alignments2.end(), score_sw);

    // Calculate best combined score here
    auto high_scores = get_best_scoring_pairs(alignments1, alignments2, mu, sigma );

    // Calculate joint MAPQ score
    int mapq1, mapq2;
    if (high_scores.size() > 1) {
        auto best_aln_pair = high_scores[0];
        auto S1 = std::get<0>(best_aln_pair);
        auto second_aln_pair = high_scores[1];
        auto S2 = std::get<0>(second_aln_pair);
        std::tie(mapq1, mapq2) = joint_mapq_from_alignment_scores(S1, S2);
    } else {
        mapq1 = 60;
        mapq2 = 60;
    }

    // append both alignments to string here
    if (max_secondary == 0) {
        auto best_aln_pair = high_scores[0];
        Alignment alignment1 = std::get<1>(best_aln_pair);
        Alignment alignment2 = std::get<2>(best_aln_pair);
        if (swap_r1r2) {
            sam.add_pair(alignment2, alignment1, record2, record1, read2.rc, read1.rc, mapq2, mapq1, is_proper_pair(alignment2, alignment1, mu, sigma), true, details);
        } else {
            sam.add_pair(alignment1, alignment2, record1, record2, read1.rc, read2.rc, mapq1, mapq2, is_proper_pair(alignment1, alignment2, mu, sigma), true, details);
        }
    } else {
        auto max_out = std::min(high_scores.size(), max_secondary);
        bool is_primary = true;
        auto best_aln_pair = high_scores[0];
        auto s_max = std::get<0>(best_aln_pair);
        for (size_t i = 0; i < max_out; ++i) {
            if (i > 0) {
                is_primary = false;
                mapq1 = 0;
                mapq2 = 0;
            }
            auto aln_pair = high_scores[i];
            auto s_score = std::get<0>(aln_pair);
            Alignment alignment1 = std::get<1>(aln_pair);
            Alignment alignment2 = std::get<2>(aln_pair);
            if (s_max - s_score < secondary_dropoff) {
                if (swap_r1r2) {
                    bool is_proper = is_proper_pair(alignment2, alignment1, mu, sigma);
                    std::array<Details, 2> swapped_details{details[1], details[0]};
                    sam.add_pair(alignment2, alignment1, record2, record1, read2.rc, read1.rc, mapq2, mapq1, is_proper, is_primary, swapped_details);
                } else {
                    bool is_proper = is_proper_pair(alignment1, alignment2, mu, sigma);
                    sam.add_pair(alignment1, alignment2, record1, record2, read1.rc, read2.rc, mapq1, mapq2, is_proper, is_primary, details);
                }
            } else {
                break;
            }
        }
    }
}

/* Compute paired-end mapping score given top alignments */
static std::pair<int, int> joint_mapq_from_high_scores(const std::vector<std::tuple<double,Alignment,Alignment>>& high_scores) {
    if (high_scores.size() <= 1) {
        return std::make_pair(60, 60);
    }
    // Calculate joint MAPQ score
    int n_mappings = high_scores.size();
    auto best_aln_pair = high_scores[0];
    auto S1 = std::get<0>(best_aln_pair);
    auto a1_m1 = std::get<1>(best_aln_pair);
    auto a1_m2 = std::get<2>(best_aln_pair);
    int a1_start_m1 = a1_m1.ref_start;
    int a1_start_m2 = a1_m2.ref_start;
    int a1_ref_id_m1 = a1_m1.ref_id;
    int a1_ref_id_m2 = a1_m2.ref_id;

    auto second_aln_pair = high_scores[1];
    auto S2 = std::get<0>(second_aln_pair);
    auto a2_m1 = std::get<1>(second_aln_pair);
    auto a2_m2 = std::get<2>(second_aln_pair);
    int a2_start_m1 = a2_m1.ref_start;
    int a2_start_m2 = a2_m2.ref_start;
    int a2_ref_id_m1 = a2_m1.ref_id;
    int a2_ref_id_m2 = a2_m2.ref_id;
    bool same_pos = (a1_start_m1 == a2_start_m1) && (a1_start_m2 == a2_start_m2);
    bool same_ref = (a1_ref_id_m1 == a2_ref_id_m1) && (a1_ref_id_m2 == a2_ref_id_m2);
    if (!same_pos || !same_ref) {
        return joint_mapq_from_alignment_scores(S1, S2);
    } else if (n_mappings > 2) {
        // individually highest alignment score was the same alignment as the joint highest score - calculate mapq relative to third best
        auto third_aln_pair = high_scores[2];
        auto S2 = std::get<0>(third_aln_pair);
        return joint_mapq_from_alignment_scores(S1, S2);
    } else {
        // there was no other alignment
        return std::make_pair(60, 60);
    }
}

// compute dropoff of the first (top) NAM
float top_dropoff(std::vector<Nam>& nams) {
    auto& n_max = nams[0];
    if (n_max.n_hits <= 2) {
        return 1.0;
    }
    if (nams.size() > 1) {
        return (float) nams[1].n_hits / n_max.n_hits;
    }
    return 0.0;
}

inline void align_PE(
    const Aligner& aligner,
    Sam &sam,
    std::vector<Nam> &nams1,
    std::vector<Nam> &nams2,
    const KSeq &record1,
    const KSeq &record2,
    int k,
    const References& references,
    std::array<Details, 2>& details,
    float dropoff,
    i_dist_est &isize_est,
    int max_tries,
    size_t max_secondary
) {
    const auto mu = isize_est.mu;
    const auto sigma = isize_est.sigma;
    Read read1(record1.seq);
    Read read2(record2.seq);
    double secondary_dropoff = 2 * aligner.parameters.mismatch + aligner.parameters.gap_open;

    if (nams1.empty() && nams2.empty()) {
         // None of the reads have any NAMs
        sam.add_unmapped_pair(record1, record2);
        return;
    }

    if (!nams1.empty() && nams2.empty()) {
        // Only read 1 has NAMS: attempt to rescue read 2
        rescue_read(
            read2,
            read1,
            aligner,
            references,
            nams1,
            max_tries,
            dropoff,
            details,
            k,
            mu,
            sigma,
            max_secondary,
            secondary_dropoff,
            sam,
            record1,
            record2,
            false
        );
        return;
    }

    if (nams1.empty() && !nams2.empty()) {
        // Only read 2 has NAMS: attempt to rescue read 1
        rescue_read(
            read1,
            read2,
            aligner,
            references,
            nams2,
            max_tries,
            dropoff,
            details,
            k,
            mu,
            sigma,
            max_secondary,
            secondary_dropoff,
            sam,
            record2,
            record1,
            true
        );
        return;
    }

    // If we get here, both reads have NAMs
    assert(!nams1.empty() && !nams2.empty());

    if (top_dropoff(nams1) < dropoff && top_dropoff(nams2) < dropoff && is_proper_nam_pair(nams1[0], nams2[0], mu, sigma)) {
        Nam n_max1 = nams1[0];
        Nam n_max2 = nams2[0];

        bool consistent_nam1 = reverse_nam_if_needed(n_max1, read1, references, k);
        details[0].nam_inconsistent += !consistent_nam1;
        bool consistent_nam2 = reverse_nam_if_needed(n_max2, read2, references, k);
        details[1].nam_inconsistent += !consistent_nam2;

        auto alignment1 = get_alignment(aligner, n_max1, references, read1, consistent_nam1);
        details[0].tried_alignment++;
        details[0].gapped += alignment1.gapped;
        auto alignment2 = get_alignment(aligner, n_max2, references, read2, consistent_nam2);
        details[1].tried_alignment++;
        details[1].gapped += alignment2.gapped;
        int mapq1 = get_mapq(nams1, n_max1);
        int mapq2 = get_mapq(nams2, n_max2);
        bool is_proper = is_proper_pair(alignment1, alignment2, mu, sigma);
        sam.add_pair(alignment1, alignment2, record1, record2, read1.rc, read2.rc, mapq1, mapq2, is_proper, true, details);

        if ((isize_est.sample_size < 400) && ((alignment1.edit_distance + alignment2.edit_distance) < 3) && is_proper) {
            isize_est.update(std::abs(alignment1.ref_start - alignment2.ref_start));
        }
        return;
    }
    // do full search of highest scoring pair
    int tries = 0;
    double S = 0.0;

    // Get top hit counts for all locations. The joint hit count is the sum of hits of the two mates. Then align as long as score dropoff or cnt < 20

    // (score, aln1, aln2)
    std::vector<std::tuple<int,Nam,Nam>> joint_nam_scores = get_best_scoring_nam_locations(nams1, nams2, mu, sigma);
    auto nam_max = joint_nam_scores[0];
    auto max_score = std::get<0>(nam_max);

    robin_hood::unordered_map<int,Alignment> is_aligned1;
    robin_hood::unordered_map<int,Alignment> is_aligned2;
    auto n1_max = nams1[0];

    bool consistent_nam1 = reverse_nam_if_needed(n1_max, read1, references, k);
    details[0].nam_inconsistent += !consistent_nam1;
    auto a1_indv_max = get_alignment(aligner, n1_max, references, read1,
                                     consistent_nam1);
//            a1_indv_max.sw_score = -10000;
    is_aligned1[n1_max.nam_id] = a1_indv_max;
    details[0].tried_alignment++;
    details[0].gapped += a1_indv_max.gapped;
    auto n2_max = nams2[0];
    bool consistent_nam2 = reverse_nam_if_needed(n2_max, read2, references, k);
    details[1].nam_inconsistent += !consistent_nam2;
    auto a2_indv_max = get_alignment(aligner, n2_max, references, read2,
                                     consistent_nam2);
//            a2_indv_max.sw_score = -10000;
    is_aligned2[n2_max.nam_id] = a2_indv_max;
    details[1].tried_alignment++;
    details[1].gapped += a2_indv_max.gapped;

    std::string r_tmp;
//            int min_ed1, min_ed2 = 1000;
//            bool new_opt1, new_opt2 = false;
//            bool a1_is_rc, a2_is_rc;
//            int ref_start, ref_len, ref_end;
//            std::cerr << "LOOOOOOOOOOOOOOOOOOOL " << min_ed << std::endl;
    std::vector<std::tuple<double,Alignment,Alignment>> high_scores; // (score, aln1, aln2)
    for (auto &[score_, n1, n2] : joint_nam_scores) {
        float score_dropoff = (float) score_ / max_score;
        if (tries >= max_tries || score_dropoff < dropoff) {
            break;
        }

        //////// the actual testing of base pair alignment part start
        Alignment a1;
        if (n1.ref_start >= 0) {
            if (is_aligned1.find(n1.nam_id) != is_aligned1.end() ){
                a1 = is_aligned1[n1.nam_id];
            } else {
                bool consistent_nam = reverse_nam_if_needed(n1, read1, references, k);
                details[0].nam_inconsistent += !consistent_nam;
                a1 = get_alignment(aligner, n1, references, read1, consistent_nam);
                is_aligned1[n1.nam_id] = a1;
                details[0].tried_alignment++;
                details[0].gapped += a1.gapped;
            }
        } else {
            // Force SW alignment to rescue mate
            details[0].mate_rescue += rescue_mate(aligner, n2, references, read2, read1, a1, mu, sigma, k);
            details[0].tried_alignment++;
        }

        if (a1.score >  a1_indv_max.score){
            a1_indv_max = a1;
        }

        Alignment a2;
        if (n2.ref_start >= 0) {
            if (is_aligned2.find(n2.nam_id) != is_aligned2.end() ){
                a2 = is_aligned2[n2.nam_id];
            } else {
                bool consistent_nam = reverse_nam_if_needed(n2, read2, references, k);
                details[1].nam_inconsistent += !consistent_nam;
                a2 = get_alignment(aligner, n2, references, read2, consistent_nam);
                is_aligned2[n2.nam_id] = a2;
                details[1].tried_alignment++;
                details[1].gapped += a2.gapped;
            }
        } else {
            // Force SW alignment to rescue mate
            details[1].mate_rescue += rescue_mate(aligner, n1, references, read1, read2, a2, mu, sigma, k);
            details[1].tried_alignment++;
        }

        if (a2.score > a2_indv_max.score){
            a2_indv_max = a2;
        }

        bool r1_r2 = a2.is_rc && (a1.ref_start <= a2.ref_start) && ((a2.ref_start - a1.ref_start) < mu + 10*sigma); // r1 ---> <---- r2
        bool r2_r1 = a1.is_rc && (a2.ref_start <= a1.ref_start) && ((a1.ref_start - a2.ref_start) < mu + 10*sigma); // r2 ---> <---- r1

        if (r1_r2 || r2_r1) {
            float x = std::abs(a1.ref_start - a2.ref_start);
            S = (double)a1.score + (double)a2.score + log(normal_pdf(x, mu, sigma));  //* (1 - s2 / s1) * min_matches * log(s1);
//                    std::cerr << " CASE1: " << S << " " <<  log( normal_pdf(x, mu, sigma ) ) << " " << (double)a1.sw_score << " " << (double)a2.sw_score << std::endl;
        } else { // individual score
            S = (double)a1.score + (double)a2.score - 20; // 20 corresponds to a value of log( normal_pdf(x, mu, sigma ) ) of more than 5 stddevs away (for most reasonable values of stddev)
//                    std::cerr << " CASE2: " << S << " " << (double)a1.sw_score << " " << (double)a2.sw_score << std::endl;
        }

        std::tuple<double, Alignment, Alignment> aln_tuple(S, a1, a2);
        high_scores.push_back(aln_tuple);

        tries++;
    }

    // Finally, add highest scores of both mates as individually mapped
    S = (double)a1_indv_max.score + (double)a2_indv_max.score - 20; // 20 corresponds to  a value of log( normal_pdf(x, mu, sigma ) ) of more than 5 stddevs away (for most reasonable values of stddev)
    std::tuple<double, Alignment, Alignment> aln_tuple (S, a1_indv_max, a2_indv_max);
    high_scores.push_back(aln_tuple);
    std::sort(high_scores.begin(), high_scores.end(), sort_scores); // Sorting by highest score first

//            std::cerr << x << " " << mu << " " << sigma << " " << log( normal_pdf(x, mu, sigma ) ) << std::endl;
//            std::cerr << 200 << " " << 200 << " " << 30 << " " << log( normal_pdf(200, 200, 30 ) ) << std::endl;
//            std::cerr << 200 << " " << 200 << " " << 200 << " " << log( normal_pdf(200, 200, 200 ) ) << std::endl;
//            std::cerr << 350 << " " << 200 << " " << 30 << " " << log( normal_pdf(350, 200, 30 ) ) << std::endl;
//            std::cerr << 1000 << " " << 200 << " " << 200 << " " << log( normal_pdf(400, 200, 200 ) ) << std::endl;

//            for (auto hsp: high_scores){
//                auto score_ = std::get<0>(hsp);
//                auto s1_tmp = std::get<1>(hsp);
//                auto s2_tmp = std::get<2>(hsp);
//                std::cerr << "HSP SCORE: " << score_ << " " << s1_tmp.ref_start << " " << s2_tmp.ref_start << " " << s1_tmp.sw_score <<  " " << s2_tmp.sw_score << std::endl;
//            }
    int mapq1, mapq2;
    std::tie(mapq1, mapq2) = joint_mapq_from_high_scores(high_scores);

    auto best_aln_pair = high_scores[0];
    auto alignment1 = std::get<1>(best_aln_pair);
    auto alignment2 = std::get<2>(best_aln_pair);
    if (max_secondary == 0) {
        bool is_proper = is_proper_pair(alignment1, alignment2, mu, sigma);
        sam.add_pair(alignment1, alignment2, record1, record2, read1.rc, read2.rc,
                        mapq1, mapq2, is_proper, true, details);
    } else {
        int max_out = std::min(high_scores.size(), max_secondary);
        // remove eventual duplicates - comes from, e.g., adding individual best alignments above (if identical to joint best alignment)
        float s_max = std::get<0>(best_aln_pair);
        int prev_start_m1 = alignment1.ref_start;
        int prev_start_m2 = alignment2.ref_start;
        int prev_ref_id_m1 = alignment1.ref_id;
        int prev_ref_id_m2 = alignment2.ref_id;
        bool is_primary = true;
        for (int i = 0; i < max_out; ++i) {
            auto aln_pair = high_scores[i];
            alignment1 = std::get<1>(aln_pair);
            alignment2 = std::get<2>(aln_pair);
            float s_score = std::get<0>(aln_pair);
            if (i > 0) {
                is_primary = false;
                mapq1 = 255;
                mapq2 = 255;
                bool same_pos = (prev_start_m1 == alignment1.ref_start) && (prev_start_m2 == alignment2.ref_start);
                bool same_ref = (prev_ref_id_m1 == alignment1.ref_id) && (prev_ref_id_m2 == alignment2.ref_id);
                if ( same_pos && same_ref ){
                    continue;
                }
            }

            if (s_max - s_score < secondary_dropoff) {
                bool is_proper = is_proper_pair(alignment1, alignment2, mu, sigma);
                sam.add_pair(alignment1, alignment2, record1, record2, read1.rc, read2.rc,
                                mapq1, mapq2, is_proper, is_primary, details);
            } else {
                break;
            }

            prev_start_m1 = alignment1.ref_start;
            prev_start_m2 = alignment2.ref_start;
            prev_ref_id_m1 = alignment1.ref_id;
            prev_ref_id_m2 = alignment2.ref_id;
        }
    }
}

inline void get_best_map_location(std::vector<Nam> &nams1, std::vector<Nam> &nams2, i_dist_est &isize_est, Nam &best_nam1,  Nam &best_nam2 ) {
    std::vector<std::tuple<int,Nam,Nam>> joint_nam_scores = get_best_scoring_nam_locations(nams1, nams2, isize_est.mu, isize_est.sigma);
    Nam n1_joint_max, n2_joint_max, n1_indiv_max, n2_indiv_max;
    float score_joint = 0;
    float score_indiv = 0;
    best_nam1.ref_start = -1; //Unmapped until proven mapped
    best_nam2.ref_start = -1; //Unmapped until proven mapped

    if (joint_nam_scores.empty()) {
        return;
    }
    // get best joint score
    for (auto &t : joint_nam_scores) { // already sorted by descending score
        auto n1 = std::get<1>(t);
        auto n2 = std::get<2>(t);
        if ((n1.ref_start >=0) && (n2.ref_start >=0) ){ // Valid pair
            score_joint =  n1.score + n2.score;
            n1_joint_max = n1;
            n2_joint_max = n2;
            break;
        }
    }

    // get individual best scores
    if (!nams1.empty()) {
        auto n1_indiv_max = nams1[0];
        score_indiv += n1_indiv_max.score - (n1_indiv_max.score/2.0); //Penalty for being mapped individually
        best_nam1 = n1_indiv_max;
    }
    if (!nams2.empty()) {
        auto n2_indiv_max = nams2[0];
        score_indiv += n2_indiv_max.score - (n2_indiv_max.score/2.0); //Penalty for being mapped individually
        best_nam2 = n2_indiv_max;
    }
    if ( score_joint > score_indiv ){ // joint score is better than individual
        best_nam1 = n1_joint_max;
        best_nam2 = n2_joint_max;
    }

    if (isize_est.sample_size < 400 && score_joint > score_indiv) {
        isize_est.update(std::abs(n1_joint_max.ref_start - n2_joint_max.ref_start));
    }
}

/* Add a new observation */
void i_dist_est::update(int dist)
{
    if (dist >= 2000) {
        return;
    }
    const float e = dist - mu;
    mu += e / sample_size; // (1.0/(sample_size +1.0)) * (sample_size*mu + d);
    SSE += e * (dist - mu);
    if (sample_size > 1) {
        //d < 1000 ? ((sample_size +1.0)/sample_size) * ( (V*sample_size/(sample_size +1)) + ((mu-d)*(mu-d))/sample_size ) : V;
        V = SSE / (sample_size - 1.0);
    } else {
        V = SSE;
    }
    sigma = std::sqrt(V);
    sample_size = sample_size + 1.0;
    if (mu < 0) {
        std::cerr << "mu negative, mu: " << mu << " sigma: " << sigma << " SSE: " << SSE << " sample size: " << sample_size << std::endl;
    }
    if (SSE < 0) {
        std::cerr << "SSE negative, mu: " << mu << " sigma: " << sigma << " SSE: " << SSE << " sample size: " << sample_size << std::endl;
    }
}


void align_PE_read(
    const KSeq &record1,
    const KSeq &record2,
    Sam& sam,
    std::string& outstring,
    AlignmentStatistics &statistics,
    i_dist_est &isize_est,
    const Aligner &aligner,
    const MappingParameters &map_param,
    const IndexParameters& index_parameters,
    const References& references,
    const StrobemerIndex& index
) {
    std::array<Details, 2> details;

    Timer strobe_timer;
    auto query_randstrobes1 = randstrobes_query(record1.seq, index_parameters);
    auto query_randstrobes2 = randstrobes_query(record2.seq, index_parameters);
    statistics.tot_construct_strobemers += strobe_timer.duration();

    // Find NAMs
    Timer nam_timer;
    auto [nonrepetitive_fraction1, nams1] = find_nams(query_randstrobes1, index);
    auto [nonrepetitive_fraction2, nams2] = find_nams(query_randstrobes2, index);
    statistics.tot_find_nams += nam_timer.duration();
    if (map_param.rescue_level > 1) {
        Timer rescue_timer;
        if (nams1.empty() || nonrepetitive_fraction1 < 0.7) {
            nams1 = find_nams_rescue(query_randstrobes1, index, map_param.rescue_cutoff);
            details[0].nam_rescue = true;
        }

        if (nams2.empty() || nonrepetitive_fraction2 < 0.7) {
            nams2 = find_nams_rescue(query_randstrobes2, index, map_param.rescue_cutoff);
            details[1].nam_rescue = true;
        }
        statistics.tot_time_rescue += rescue_timer.duration();
    }
    details[0].nams = nams1.size();
    details[1].nams = nams2.size();

    Timer nam_sort_timer;
    std::sort(nams1.begin(), nams1.end(), score);
    std::sort(nams2.begin(), nams2.end(), score);
    statistics.tot_sort_nams += nam_sort_timer.duration();

    Timer extend_timer;
    if (!map_param.is_sam_out) {
        Nam nam_read1;
        Nam nam_read2;
        get_best_map_location(nams1, nams2, isize_est,
                              nam_read1,
                              nam_read2);
        output_hits_paf_PE(outstring, nam_read1, record1.name,
                           references,
                           index_parameters.syncmer.k,
                           record1.seq.length());
        output_hits_paf_PE(outstring, nam_read2, record2.name,
                           references,
                           index_parameters.syncmer.k,
                           record2.seq.length());
    } else {
        align_PE(aligner, sam, nams1, nams2, record1,
                 record2,
                 index_parameters.syncmer.k,
                 references, details,
                 map_param.dropoff_threshold, isize_est, map_param.max_tries, map_param.max_secondary);
    }
    statistics.tot_extend += extend_timer.duration();
    statistics += details[0];
    statistics += details[1];
}


void align_SE_read(
    const KSeq &record,
    Sam& sam,
    std::string &outstring,
    AlignmentStatistics &statistics,
    const Aligner &aligner,
    const MappingParameters &map_param,
    const IndexParameters& index_parameters,
    const References& references,
    const StrobemerIndex& index
) {
    Details details;
    Timer strobe_timer;
    auto query_randstrobes = randstrobes_query(record.seq, index_parameters);
    statistics.tot_construct_strobemers += strobe_timer.duration();

    // Find NAMs
    Timer nam_timer;
    auto [nonrepetitive_fraction, nams] = find_nams(query_randstrobes, index);
    statistics.tot_find_nams += nam_timer.duration();

    if (map_param.rescue_level > 1) {
        Timer rescue_timer;
        if (nams.empty() || nonrepetitive_fraction < 0.7) {
            details.nam_rescue = true;
            nams = find_nams_rescue(query_randstrobes, index, map_param.rescue_cutoff);
        }
        statistics.tot_time_rescue += rescue_timer.duration();
    }

    details.nams = nams.size();
    Timer nam_sort_timer;
    std::sort(nams.begin(), nams.end(), score);
    statistics.tot_sort_nams += nam_sort_timer.duration();

    Timer extend_timer;
    if (!map_param.is_sam_out) {
        output_hits_paf(outstring, nams, record.name, references, index_parameters.syncmer.k,
                        record.seq.length());
    } else {
        align_SE(
            aligner, sam, nams, record, index_parameters.syncmer.k,
            references, details, map_param.dropoff_threshold, map_param.max_tries,
            map_param.max_secondary
        );
    }
    statistics.tot_extend += extend_timer.duration();
    statistics += details;
}
