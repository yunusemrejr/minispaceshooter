/* ml.cpp — implementation of the from-scratch learners. */
#include "ml.hpp"
#include "limits.hpp"

#include <cmath>
#include <cstring>

namespace mss {
namespace ml {

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int dimension(int n, int max) { return n < 1 ? 1 : (n > max ? max : n); }

static bool valid_sample(const float *x, int n)
{
    if (!x || n <= 0) return false;
    for (int i = 0; i < n; ++i) if (!std::isfinite(x[i])) return false;
    return true;
}

/* --------------------------------------------------------------------- mlp */
void Mlp::init(int n_in, int n_hid, int n_out, Rng &rng, float learning_rate)
{
    nin = dimension(n_in, MAX_IN);
    nhid = dimension(n_hid, MAX_HID);
    nout = dimension(n_out, MAX_OUT);
    lr = std::isfinite(learning_rate) ? clampf(learning_rate, 0.0f, 1.0f) : 0.01f;
    momentum = 0.6f;
    w_clamp = 3.0f;
    last_loss = 0.0f;
    steps = 0;
    std::memset(mw1, 0, sizeof(mw1));
    std::memset(mb1, 0, sizeof(mb1));
    std::memset(mw2, 0, sizeof(mw2));
    std::memset(mb2, 0, sizeof(mb2));
    /* He-style scaling keeps early activations in tanh's linear region. */
    float s1 = 1.0f / std::sqrt((float)nin);
    float s2 = 1.0f / std::sqrt((float)nhid);
    for (int j = 0; j < nhid; ++j) {
        for (int i = 0; i < nin; ++i) w1[j][i] = rng.gauss() * s1;
        b1[j] = 0.0f;
    }
    for (int k = 0; k < nout; ++k) {
        for (int j = 0; j < nhid; ++j) w2[k][j] = rng.gauss() * s2 * 0.5f;
        b2[k] = 0.0f;
    }
    for (int j = 0; j < nhid; ++j) h[j] = 0.0f;
    for (int k = 0; k < nout; ++k) out[k] = 0.0f;
}

const float *Mlp::forward(const float *x)
{
    for (int j = 0; j < nhid; ++j) {
        float z = b1[j];
        for (int i = 0; i < nin; ++i) z += w1[j][i] * x[i];
        h[j] = std::tanh(z);
    }
    for (int k = 0; k < nout; ++k) {
        float z = b2[k];
        for (int j = 0; j < nhid; ++j) z += w2[k][j] * h[j];
        out[k] = z;
    }
    return out;
}

void Mlp::softmax(float *probs, int n) const
{
    if (n > nout) n = nout;
    if (!probs || n <= 0) return;
    float mx = out[0];
    for (int k = 1; k < n; ++k) mx = out[k] > mx ? out[k] : mx;
    float sum = 0.0f;
    for (int k = 0; k < n; ++k) {
        float e = std::exp(out[k] - mx);
        probs[k] = e;
        sum += e;
    }
    if (sum <= 0.0f) sum = 1.0f;
    for (int k = 0; k < n; ++k) probs[k] /= sum;
}

void Mlp::nudge(float &w, float &m, float grad)
{
    m = momentum * m - lr * grad;
    w += m;
    w = clampf(w, -w_clamp, w_clamp);
}

void Mlp::backprop(const float *x, const float *dout)
{
    /* Gradients are accumulated first and applied second, so the backward
     * pass always uses the weights that produced the forward activations. */
    float g2[MAX_OUT][MAX_HID];
    float gb2[MAX_OUT];
    float g1[MAX_HID][MAX_IN];
    float gb1[MAX_HID];

    for (int k = 0; k < nout; ++k) {
        gb2[k] = dout[k];
        for (int j = 0; j < nhid; ++j) g2[k][j] = dout[k] * h[j];
    }
    for (int j = 0; j < nhid; ++j) {
        float dh = 0.0f;
        for (int k = 0; k < nout; ++k) dh += dout[k] * w2[k][j];
        float dz = dh * (1.0f - h[j] * h[j]); /* tanh' */
        gb1[j] = dz;
        for (int i = 0; i < nin; ++i) g1[j][i] = dz * x[i];
    }

    for (int k = 0; k < nout; ++k) {
        for (int j = 0; j < nhid; ++j) nudge(w2[k][j], mw2[k][j], g2[k][j]);
        nudge(b2[k], mb2[k], gb2[k]);
    }
    for (int j = 0; j < nhid; ++j) {
        for (int i = 0; i < nin; ++i) nudge(w1[j][i], mw1[j][i], g1[j][i]);
        nudge(b1[j], mb1[j], gb1[j]);
    }
}

float Mlp::train_mse(const float *x, const float *target)
{
    if (!valid_sample(x, nin) || !valid_sample(target, nout)) return 0.0f;
    forward(x);
    float d[MAX_OUT];
    float loss = 0.0f;
    for (int k = 0; k < nout; ++k) {
        float e = out[k] - target[k];
        loss += e * e;
        d[k] = 2.0f * e / (float)nout;
    }
    loss /= (float)nout;
    last_loss = loss;
    increment(steps);
    backprop(x, d);
    return loss;
}

float Mlp::train_policy(const float *x, int action, float advantage, float entropy_bonus)
{
    if (!valid_sample(x, nin) || action < 0 || action >= nout ||
        !std::isfinite(advantage) || !std::isfinite(entropy_bonus)) return 0.0f;
    advantage = clampf(advantage, -1.0f, 1.0f);
    entropy_bonus = clampf(entropy_bonus, 0.0f, 1.0f);
    forward(x);
    float p[MAX_OUT];
    softmax(p, nout);
    if (action < 0 || action >= nout) return 0.0f;

    float ent = 0.0f;
    for (int k = 0; k < nout; ++k) {
        ent -= p[k] * std::log(clampf(p[k], 1e-8f, 1.0f));
    }

    /* Exact softmax Jacobian for L = -advantage*log p(action) - bonus*H(p).
     * The shared entropy term makes the logit gradients sum to zero. */
    float dout[MAX_OUT];
    for (int k = 0; k < nout; ++k) {
        float pk = p[k];
        float onehot = (k == action) ? 1.0f : 0.0f;
        float g_policy = -advantage * (onehot - pk);
        float g_entropy = entropy_bonus * pk * (std::log(clampf(pk, 1e-8f, 1.0f)) + ent);
        dout[k] = g_policy + g_entropy;
    }
    increment(steps);
    last_loss = -advantage * std::log(clampf(p[action], 1e-8f, 1.0f)) - entropy_bonus * ent;
    backprop(x, dout);
    return ent;
}

float Mlp::weight_l1() const
{
    float s = 0.0f;
    for (int j = 0; j < nhid; ++j) {
        for (int i = 0; i < nin; ++i) s += std::fabs(w1[j][i]);
        s += std::fabs(b1[j]);
    }
    for (int k = 0; k < nout; ++k) {
        for (int j = 0; j < nhid; ++j) s += std::fabs(w2[k][j]);
        s += std::fabs(b2[k]);
    }
    return s;
}

/* ------------------------------------------------------------------- logit */
void Logit::init(int n_in, float learning_rate)
{
    n = dimension(n_in, MAX_IN);
    lr = std::isfinite(learning_rate) ? clampf(learning_rate, 0.0f, 1.0f) : 0.25f;
    b = 0.0f;
    mb = 0.0f;
    steps = 0;
    last_prob = 0.5f;
    for (int i = 0; i < MAX_IN; ++i) {
        w[i] = 0.0f;
        m[i] = 0.0f;
    }
    loss.init(0.69f, 0.02f);
}

float Logit::prob(const float *x) const
{
    float z = b;
    for (int i = 0; i < n; ++i) z += w[i] * x[i];
    /* numerically safe sigmoid */
    if (z >= 0.0f) {
        float e = std::exp(-z);
        return 1.0f / (1.0f + e);
    }
    float e = std::exp(z);
    return e / (1.0f + e);
}

float Logit::train(const float *x, float y)
{
    if (!valid_sample(x, n) || !std::isfinite(y)) return loss.v;
    y = clampf(y, 0.0f, 1.0f);
    float p = prob(x);
    last_prob = p;
    float g = p - y; /* dL/dz for binary cross-entropy */
    if (std::fabs(g) > 1e-4f) {
        for (int i = 0; i < n; ++i) {
            m[i] = 0.5f * m[i] - lr * g * x[i];
            w[i] = clampf(w[i] + m[i], -w_clamp, w_clamp);
        }
        mb = 0.5f * mb - lr * g;
        b = clampf(b + mb, -w_clamp, w_clamp);
    }
    increment(steps);
    float yy = y;
    float pp = clampf(p, 1e-4f, 1.0f - 1e-4f);
    return loss.update(-(yy * std::log(pp) + (1.0f - yy) * std::log(1.0f - pp)));
}

/* ------------------------------------------------------------------ bandit */
void Bandit::init(int arms)
{
    n = dimension(arms, MAX_ARMS);
    total = 0;
    evidence = 0;
    last_arm = 0;
    for (int i = 0; i < MAX_ARMS; ++i) {
        pulls[i] = 0;
        mean[i] = 0.0f;
        last_reward[i] = 0.0f;
    }
}

float Bandit::confidence(int arm) const
{
    if (arm < 0 || arm >= n) return 0.0f;
    if (pulls[arm] == 0) return 1e9f; /* untried arms look infinitely promising */
    return std::sqrt(std::log((float)evidence + 1.0f) / (float)pulls[arm]);
}

int Bandit::select(Rng &rng, float explore)
{
    if (n <= 0) return 0;
    int untried = 0;
    for (int i = 0; i < n; ++i) untried += (pulls[i] == 0) ? 1 : 0;
    if (untried > 0) {
        int pick = rng.range(untried);
        for (int i = 0; i < n; ++i) {
            if (pulls[i] == 0 && pick-- == 0) return i;
        }
    }
    int best = 0;
    float best_score = -1e30f;
    for (int i = 0; i < n; ++i) {
        float score = mean[i] + explore * confidence(i);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

void Bandit::update(int arm, float reward)
{
    if (arm < 0 || arm >= n || !std::isfinite(reward)) return;
    if (reward < -1.0f) reward = -1.0f;
    if (reward > 1.0f) reward = 1.0f;
    /* Forget old confidence as well as old rewards: the player keeps learning. */
    if (evidence >= 512) {
        evidence = 0;
        for (int i = 0; i < n; ++i) {
            if (pulls[i] > 1) pulls[i] = (pulls[i] + 1) / 2;
            evidence += pulls[i];
        }
    }
    pulls[arm]++;
    evidence++;
    increment(total);
    last_arm = arm;
    last_reward[arm] = reward;
    float step = 1.0f / (float)pulls[arm];
    if (step < 0.05f) step = 0.05f;
    mean[arm] += (reward - mean[arm]) * step;
}

float Bandit::best_mean() const
{
    float b = -1e30f;
    for (int i = 0; i < n; ++i) b = mean[i] > b ? mean[i] : b;
    return b;
}

int Bandit::best_arm() const
{
    int b = 0;
    for (int i = 1; i < n; ++i) {
        if (mean[i] > mean[b]) b = i;
    }
    return b;
}

/* ------------------------------------------------------------------ kmeans */
void KMeans::init(int k_clusters, int dims, Rng &rng, const float *sample)
{
    k = dimension(k_clusters, MAX_CLUSTERS);
    dim = dimension(dims, MAX_IN);
    inertia = 0.0f;
    mean_distance = 0.0f;
    last = 0;
    for (int i = 0; i < k; ++i) {
        count[i] = 1;
        for (int d = 0; d < dim; ++d) {
            /* First centroid is the live sample; the rest start nearby so the
             * clusters can separate without a stored dataset. */
            float v = sample && std::isfinite(sample[d]) ? sample[d] : 0.0f;
            c[i][d] = v + (i == 0 ? 0.0f : rng.sym() * 0.35f);
        }
    }
}

int KMeans::assign(const float *x)
{
    if (k <= 0 || !valid_sample(x, dim)) return -1;
    int best = 0;
    float best_d = 1e30f;
    for (int i = 0; i < k; ++i) {
        float d = 0.0f;
        for (int j = 0; j < dim; ++j) {
            float e = x[j] - c[i][j];
            d += e * e;
        }
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    last = best;
    inertia = best_d;
    return best;
}

void KMeans::update(const float *x)
{
    int i = assign(x); /* also sets inertia = squared distance to centroid i */
    if (i < 0) return;
    int samples = 0;
    for (int j = 0; j < k; ++j) samples += count[j];
    if (samples >= 1024) {
        for (int j = 0; j < k; ++j) count[j] = (count[j] + 1) / 2;
    }
    count[i]++;
    float step = 1.0f / (float)count[i];
    if (step < 0.01f) step = 0.01f;
    for (int j = 0; j < dim; ++j) c[i][j] += (x[j] - c[i][j]) * step;
    /* Keep two honest, clearly defined quantities: inertia (squared distance
     * of the last assignment) and a decayed average Euclidean distance. */
    mean_distance += (std::sqrt(inertia) - mean_distance) * 0.05f;
}

int KMeans::dominant() const
{
    int b = 0;
    for (int i = 1; i < k; ++i) {
        if (count[i] > count[b]) b = i;
    }
    return b;
}

} /* namespace ml */
} /* namespace mss */
