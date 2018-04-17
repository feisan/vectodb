#include "vectodb.h"

#include "AutoTune.h"
#include "IndexFlat.h"
#include "index_io.h"

#include <boost/filesystem.hpp>
#include <glog/logging.h>
#include <omp.h>

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <stdio.h>
#include <string>
#include <sys/time.h>
#include <unordered_map>
#include <vector>

using namespace std;
namespace fs = boost::filesystem;

const long MAX_NTRAIN = 160000L; //the number of training points which IVF4096 needs for 1M dataset

struct DbState {
    DbState()
        : ntrain(0L)
        , index(nullptr)
    {
    }
    ~DbState()
    {
        delete index;
        fs_base.close();
    }

    std::fstream fs_base;
    vector<float> base;
    vector<long> uids;
    unordered_map<long, long> uid2num;
    long ntrain; // the number of training points of the index
    faiss::Index* index;
};

VectoDB::VectoDB(const char* work_dir_in, long dim_in, int metric_type_in, const char* index_key_in, const char* query_params_in)
    : work_dir(work_dir_in)
    , dim(dim_in)
    , metric_type(metric_type_in)
    , index_key(index_key_in)
    , query_params(query_params_in)
{
    //Sets the number of threads in subsequent parallel regions.
    omp_set_num_threads(1);

    static_assert(sizeof(float) == 4, "sizeof(float) must be 4");
    static_assert(sizeof(long) > 4, "sizeof(long) must be larger than 4");

    fs::path dir{ fs::absolute(work_dir_in) };
    work_dir = dir.string().c_str();

    auto st{ std::make_unique<DbState>() }; //Make DbState be exception safe
    state = std::move(st); // equivalent to state.reset(st.release());
    state->base.reserve(dim * 1000000);
    state->uids.reserve(1000000);
    fs::create_directories(dir);
    //filename spec: base.fvecs, <index_key>.<ntrain>.index
    //line spec of base.fvecs: <uid> {<dim>}<float>
    const string fp_base = getBaseFp();
    //Loading database
    //https://stackoverflow.com/questions/31483349/how-can-i-open-a-file-for-reading-writing-creating-it-if-it-does-not-exist-w
    state->fs_base.exceptions(std::ios::failbit | std::ios::badbit);
    state->fs_base.open(fp_base, std::fstream::out | std::fstream::app); //create file if not exist, otherwise do nothing
    state->fs_base.close();
    state->fs_base.open(fp_base, std::fstream::in | std::fstream::out | std::fstream::binary);
    long len_line = sizeof(long) + dim * sizeof(float);
    long len_f = fs::file_size(fp_base); //equivalent to "fs_base.seekp(0, ios_base::end); long len_f = fs_base.tellp();"
    if (len_f % len_line != 0) {
        ostringstream oss;
        oss << fp_base << " file size " << len_f << " is not multiple of line length " << len_line;
        throw std::length_error(oss.str());
    }
    long num_line = len_f / len_line;
    if (num_line > 0) {
        LOG(INFO) << "Loading base " << fp_base;
        state->base.resize(num_line * dim);
        state->uids.resize(num_line);
        vector<char> buf(len_line);
        for (long i = 0; i < num_line; i++) {
            state->fs_base.read(&buf[0], len_line);
            long uid = *(long*)&buf[0];
            state->uids[i] = uid;
            state->uid2num[uid] = i;
            memcpy(&state->base[i * dim], &buf[sizeof(long)], dim * sizeof(float));
        }
    }
    state->fs_base.seekp(0, ios_base::end); //a particular libstdc++ implementation may use a single pointer for both seekg and seekp.
    long ntrain = getIndexFpNtrain();
    if (num_line >= ntrain && ntrain > 0) {
        //Loading index
        const string& fp_index = getIndexFp(ntrain);
        LOG(INFO) << "Loading index " << fp_index;
        state->index = faiss::read_index(fp_index.c_str());
        state->ntrain = ntrain;
    }
    buildFlatIndex(state->index, state->uids.size(), &state->base[0]);
    google::FlushLogFiles(google::INFO);
}

VectoDB::~VectoDB()
{
}

/**
 * Writer methods
 */

void VectoDB::ActivateIndex(faiss::Index* index, long ntrain)
{
    if (index == nullptr)
        return;
    if (strcmp(index_key, "Flat")) {
        if (state->ntrain != 0)
            fs::remove(getIndexFp(state->ntrain));
        // Output index
        faiss::write_index(index, getIndexFp(ntrain).c_str());
    }
    delete state->index;
    state->ntrain = ntrain;
    state->index = index;
}

void VectoDB::AddWithIds(long nb, const float* xb, const long* xids)
{
    assert(state->base.size() == dim * state->uids.size());
    long len_line = sizeof(long) + dim * sizeof(float);
    long len_buf = nb * len_line;
    std::vector<char> buf(len_buf);
    for (long i = 0; i < nb; i++) {
        memcpy(&buf[i * len_line], &xids[i], sizeof(long));
        memcpy(&buf[i * len_line + sizeof(long)], &xb[i * dim], dim * sizeof(float));
    }
    state->fs_base.write(&buf[0], len_buf);

    long nb2 = state->uids.size();
    state->base.resize((nb + nb2) * dim);
    state->uids.resize(nb + nb2);
    memcpy(&state->base[nb2 * dim], xb, nb * dim * sizeof(float));
    memcpy(&state->uids[nb2], xb, nb * sizeof(long));
    buildFlatIndex(state->index, nb + nb2, xb);
}

void VectoDB::buildFlatIndex(faiss::Index*& index, long nb, const float* xb)
{
    if (0 == strcmp(index_key, "Flat")) {
        // Build index for Flat directly. Don't need TryBuildIndex, BuildIndex, ActivateIndex.
        if (index) {
            if (dynamic_cast<faiss::IndexFlat*>(state->index) == nullptr) {
                delete index;
                index = faiss::index_factory(dim, index_key, metric_type == 0 ? faiss::METRIC_INNER_PRODUCT : faiss::METRIC_L2);
            }
        } else {
            index = faiss::index_factory(dim, index_key, metric_type == 0 ? faiss::METRIC_INNER_PRODUCT : faiss::METRIC_L2);
        }
        // Indexing database
        index->add(nb, xb);
    }
}

/*
void VectoDB::UpdateWithIds(long nb, const float* xb, const long* xids)
{
    throw "TODO";
}
*/

/**
 * Read methods
 */

void VectoDB::TryBuildIndex(long exhaust_threshold, faiss::Index*& index, long& ntrain) const
{
    if ((long)state->uids.size() - getIndexSize() <= exhaust_threshold)
        return;
    BuildIndex(index, ntrain);
}

void VectoDB::BuildIndex(faiss::Index*& index_out, long& ntrain) const
{
    assert(state->base.size() == dim * state->uids.size());

    // Prepareing index
    LOG(INFO) << "BuildIndex " << work_dir << ". dim=" << dim << ", index_key=\"" << index_key << "\", metric=" << metric_type;
    faiss::Index* index = nullptr;

    long nb = state->uids.size();
    if (strcmp(index_key, "Flat")) {
        long nt = std::min(nb, std::max(nb / 10, MAX_NTRAIN));
        if (nt == state->ntrain) {
            assert(state->index != nullptr);
            long& ntotal = state->index->ntotal;
            if (nb == ntotal) {
                LOG(INFO) << "Nothing to do since ntrain " << nt << " and ntotal " << ntotal << " are unchanged";
                index_out = nullptr;
            } else {
                LOG(INFO) << "Reuse current index since ntrain " << nt << " is unchanged";
                index = faiss::read_index(getIndexFp(nt).c_str());
                LOG(INFO) << "Adding " << nb - ntotal << " vectors to index, ntotal increased from " << ntotal << " to " << nb;
                index->add(nb - ntotal, &state->base[ntotal * dim]);
                index_out = index;
            }
        } else {
            index = faiss::index_factory(dim, index_key, metric_type == 0 ? faiss::METRIC_INNER_PRODUCT : faiss::METRIC_L2);
            // Training
            LOG(INFO) << "Training on " << nt << " vectors";
            index->train(nt, &state->base[0]);

            // selected_params is cached auto-tuning result.
            faiss::ParameterSpace params;
            params.initialize(index);
            params.set_index_parameters(index, query_params);

            // Indexing database
            LOG(INFO) << "Indexing " << nb << " vectors";
            index->add(nb, &state->base[0]);
            index_out = index;
        }
        ntrain = nt;
    } else {
        index = faiss::index_factory(dim, index_key, metric_type == 0 ? faiss::METRIC_INNER_PRODUCT : faiss::METRIC_L2);
        // Indexing database
        LOG(INFO) << "Indexing " << nb << " vectors";
        index->add(nb, &state->base[0]);
        index_out = index;
        ntrain = 0;
    }
    LOG(INFO) << "BuildIndex " << work_dir << " done";
    google::FlushLogFiles(google::INFO);
}

void VectoDB::Search(long nq, const float* xq, float* distances, long* xids) const
{
    // output buffers
    long k = 100;
    float* D = new float[nq * k];
    faiss::Index::idx_t* I = new faiss::Index::idx_t[nq * k];

    if (state->index) {
        // Perform a search
        state->index->search(nq, xq, k, D, I);

        // Refine result
        if (dynamic_cast<faiss::IndexFlat*>(state->index) == nullptr) {
            float* xb2 = new float[dim * k];
            float* D2 = new float[k];
            faiss::Index::idx_t* I2 = new faiss::Index::idx_t[k];
            for (int i = 0; i < nq; i++) {
                faiss::Index* index2 = faiss::index_factory(dim, "Flat");
                for (int j = 0; j < k; j++)
                    memcpy(xb2 + j * dim, &state->base[I[i * k + j] * dim], sizeof(float) * dim);
                index2->add(k, xb2);
                index2->search(1, xq + i * dim, k, D2, I2);
                delete index2;
                distances[i] = D2[0];
                xids[i] = I[i * k + I2[0]];
            }
            delete[] xb2;
            delete[] D2;
            delete[] I2;
        } else {
            for (int i = 0; i < nq; i++) {
                distances[i] = D[i * k];
                xids[i] = I[i * k];
            }
        }
    }
    long index_size = getIndexSize();
    if (index_size < (long)state->uids.size()) {
        assert(state->index == nullptr || dynamic_cast<faiss::IndexFlat*>(state->index) == nullptr);
        faiss::Index* index2 = faiss::index_factory(dim, "Flat");
        float* xb2 = &state->base[index_size * dim];
        long nb2 = state->uids.size() - index_size;
        index2->add(nb2, xb2);
        index2->search(nq, xq, k, D, I);
        delete index2;
        for (int i = 0; i < nq; i++) {
            if (0 == index_size || distances[i] > D[i * k]) {
                distances[i] = D[i * k];
                xids[i] = I[i * k];
            }
        }
    }
    delete[] D;
    delete[] I;
}

std::string VectoDB::getBaseFp() const
{
    ostringstream oss;
    oss << work_dir << "/base.fvecs";
    return oss.str();
}

std::string VectoDB::getIndexFp(long ntrain) const
{
    ostringstream oss;
    oss << work_dir << "/" << index_key << "." << ntrain << ".index";
    return oss.str();
}

long VectoDB::getIndexFpNtrain() const
{
    long max_ntrain = 0;
    fs::path fp_index;
    string prefix(index_key);
    prefix.append(".");
    const string suffix(".index");
    for (auto ent = fs::directory_iterator(work_dir); ent != fs::directory_iterator(); ent++) {
        const fs::path& p = ent->path();
        if (fs::is_regular_file(p)) {
            const string fn = p.filename().string();
            if (fn.length() >= suffix.length()
                && 0 == fn.compare(fn.length() - suffix.length(), suffix.length(), suffix)
                && 0 == fn.compare(0, prefix.length(), prefix)) {
                long ntrain = std::stol(fn.substr(prefix.length(), fn.length() - prefix.length() - suffix.length()));
                if (ntrain > max_ntrain) {
                    max_ntrain = ntrain;
                    fp_index = p;
                }
            }
        }
    }
    return max_ntrain;
}

long VectoDB::getIndexSize() const
{
    return (state->index == nullptr) ? 0 : state->index->ntotal;
}

void VectoDB::ClearWorkDir(const char* work_dir)
{
    ostringstream oss;
    oss << work_dir << "/base.fvecs";
    fs::remove(oss.str());

    const string suffix(".index");
    for (auto ent = fs::directory_iterator(work_dir); ent != fs::directory_iterator(); ent++) {
        const fs::path& p = ent->path();
        if (fs::is_regular_file(p)) {
            const string fn = p.filename().string();
            if (fn.length() >= suffix.length()
                && 0 == fn.compare(fn.length() - suffix.length(), suffix.length(), suffix)) {
                fs::remove(p);
            }
        }
    }
}
