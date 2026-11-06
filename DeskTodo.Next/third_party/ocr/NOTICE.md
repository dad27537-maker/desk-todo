# Offline OCR components

DeskTodo's native OCR pipeline uses PP-OCRv6 medium detection and recognition
models from PaddleOCR, exported to ONNX by RapidAI/RapidOCR. Model normalization
and CTC dictionary conventions follow those projects' published inference code.
The integration and image/window handling live in DeskTodo's own source files.

- PaddleOCR / PP-OCRv6: https://github.com/PaddlePaddle/PaddleOCR
  (Apache-2.0; bundled `PaddleOCR-LICENSE`).
- RapidOCR model exports: https://github.com/RapidAI/RapidOCR
  (Apache-2.0). Weights are pinned to ModelScope revision `v3.10.0` and checked
  against the SHA256 values in RapidOCR's model manifest.
- Microsoft ONNX Runtime 1.31.0: https://github.com/microsoft/onnxruntime
  (MIT; bundled `ONNXRuntime-LICENSE` and `ONNXRuntime-ThirdPartyNotices.txt`).
- OpenCV 4.12.0: https://github.com/opencv/opencv
  (Apache-2.0; bundled `OpenCV-LICENSE` and `OpenCV-licenses/`).

SDKs, model weights, and native DLLs are downloaded by `setup-ocr.ps1`; they are
not stored in Git. The application performs OCR locally and makes no network
requests for screenshots or model inference.
