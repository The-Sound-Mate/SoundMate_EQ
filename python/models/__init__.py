"""Trained ML model artifacts (reserved).

Future ML-based taggers / genre classifiers land here as `.onnx` / `.pt` /
`.joblib` files, loaded by wrappers in this package. Kept as a Python package
so loaders in `tagging/` and `eq/` can import from `models.<wrapper>`.
"""
