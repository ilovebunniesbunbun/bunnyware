#pragma once

inline void tint_highlight(float* color, float alpha, const float* tint)
{
    if (tint && (color[0] + color[1] + color[2] > 0.001f)) {
        color[0] = tint[0];
        color[1] = tint[1];
        color[2] = tint[2];
    }
    color[3] *= alpha * (tint ? tint[3] : 1.0f);
}
