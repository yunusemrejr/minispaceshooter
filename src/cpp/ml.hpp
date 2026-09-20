/* ml.hpp — the game's own machine learning, written from scratch in C++17.
 *
 * Four small algorithms, all fixed-capacity and allocation-free so a run costs
 * a constant, tiny amount of RAM:
 *
 *   Mlp     two-layer network (tanh hidden, linear out) used as a *shared*
 *           enemy policy trained online with bounded policy gradient.
 *   Logit   binary logistic regression, used twice:
 *             - intent predictor: where will the player be in a moment?
 *             - stress model:     is the player about to be overwhelmed?
 *   Bandit  UCB1 over the director's "trick" repertoire.
 *   KMeans  online k-means characterising the player's style (constant memory,
 *           one pass per frame, no stored dataset).
 *
 * Nothing here is allowed to become unbounded: every learner clamps its
 * weights and its step size, so the game can never train its way past the
 * fairness caps enforced by the director.
 */
#ifndef MINI_ML_HPP
#define MINI_ML_HPP

#include <cstdint>

#include "rng.hpp"

namespace mss {
namespace ml {

constexpr int MAX_IN = 12;
constexpr int MAX_HID = 16;
constexpr int MAX_OUT = 8;
constexpr int MAX_ARMS = 12;
constexpr int MAX_CLUSTERS = 4;

/* --------------------------------------------------------------------- ema */
struct Ema {
    float v;
    float a;
    void init(float initial, float alpha)
    {
        v = initial;
        a = alpha;
    }
    float update(float x)
    {
        v += (x - v) * a;
        return v;
    }
};

/* --------------------------------------------------------------------- mlp */
struct Mlp {
    int nin = 0, nhid = 0, nout = 0;
    float w1[MAX_HID][MAX_IN];
    float b1[MAX_HID];
    float w2[MAX_OUT][MAX_HID];
    float b2[MAX_OUT];
    float mw1[MAX_HID][MAX_IN];
    float mb1[MAX_HID];
    float mw2[MAX_OUT][MAX_HID];
    float mb2[MAX_OUT];
    float h[MAX_HID];   /* hidden activations, filled by forward() */
    float out[MAX_OUT]; /* linear outputs, filled by forward() */
    float lr = 0.01f;
    float momentum = 0.6f;
    float w_clamp = 3.0f;
    float last_loss = 0.0f;
    uint32_t steps = 0;

    void init(int n_in, int n_hid, int n_out, Rng &rng, float learning_rate = 0.01f);
    const float *forward(const float *x);
    void softmax(float *probs, int n) const;
    /* One SGD step on mean-squared error; returns the sample loss. */
    float train_mse(const float *x, const float *target);
    /* Bounded policy-gradient step for a softmax head. Returns entropy bonus. */
    float train_policy(const float *x, int action, float advantage, float entropy_bonus);
    int params() const { return nin * nhid + nhid + nout * nhid + nout; }
    float weight_l1() const;

  private:
    void backprop(const float *x, const float *dout);
    void nudge(float &w, float &m, float grad);
};

/* ------------------------------------------------------------------- logit */
struct Logit {
    int n = 0;
    float w[MAX_IN];
    float m[MAX_IN];
    float b = 0.0f;
    float mb = 0.0f;
    float lr = 0.25f;
    float w_clamp = 4.0f;
    Ema loss;
    uint32_t steps = 0;

    void init(int n_in, float learning_rate = 0.25f);
    float prob(const float *x) const;
    float train(const float *x, float y);
    float last_prob = 0.5f;
};

/* ------------------------------------------------------------------ bandit */
struct Bandit {
    int n = 0;
    int pulls[MAX_ARMS];
    float mean[MAX_ARMS];   /* running average reward */
    float last_reward[MAX_ARMS];
    uint32_t total = 0;         /* lifetime updates, saturating */
    int evidence = 0;          /* decayed pull total for exploration */
    int last_arm = 0;

    void init(int arms);
    /* UCB1.  `explore` scales the confidence bonus (director turns it down
     * as it becomes confident, which is what makes it settle instead of flail). */
    int select(Rng &rng, float explore);
    void update(int arm, float reward);
    float value(int arm) const { return arm >= 0 && arm < n ? mean[arm] : 0.0f; }
    float best_mean() const;
    int best_arm() const;
    float confidence(int arm) const;
};

/* ------------------------------------------------------------------ kmeans */
struct KMeans {
    int k = 0, dim = 0;
    float c[MAX_CLUSTERS][MAX_IN];
    int count[MAX_CLUSTERS];
    float inertia = 0.0f;       /* squared distance to the assigned centroid */
    float mean_distance = 0.0f; /* decayed average Euclidean distance to it */
    int last = 0;

    void init(int k_clusters, int dims, Rng &rng, const float *sample);
    int assign(const float *x);
    /* Bounded history keeps centroids responsive when the player's style changes. */
    void update(const float *x);
    int dominant() const;
};

} /* namespace ml */
} /* namespace mss */

#endif /* MINI_ML_HPP */
