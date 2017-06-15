package facebook.f8demo;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.AsyncTask;
import android.os.Handler;
import android.os.HandlerThread;
import android.support.annotation.NonNull;
import android.util.Size;
import android.support.v4.app.ActivityCompat;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.Window;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;
import java.nio.ByteBuffer;
import java.util.Arrays;

import static android.view.View.SYSTEM_UI_FLAG_IMMERSIVE;
public class ClassifyCamera extends AppCompatActivity {
    private static final String TAG = "F8DEMO";
    private static final int REQUEST_CAMERA_PERMISSION = 200;

    private static int w =0 ;
    private static int h =0 ;
    private static int rowStride ;
    private static int pixelStride;
    private static byte[] Y ;
    private static  byte[] U ;
    private static byte[] V ;
    private byte[][] yuvBytes;
    private int[] rgbBytes=null;

    private TextureView textureView;
    private String cameraId;
    protected CameraDevice cameraDevice;
    protected CameraCaptureSession cameraCaptureSessions;
    protected CaptureRequest.Builder captureRequestBuilder;
    private Size imageDimension;
    private Handler mBackgroundHandler;
    private HandlerThread mBackgroundThread;
    private TextView tv;
    private String predictedClass = "none";
    private AssetManager mgr;
    private boolean processing = false;
    private Image image = null;
    private boolean run_HWC = false;
    private static  SurfaceTexture texture;
    private static ImageReader reader;
    private static Button capture;
    private static int[] mRgbBuffer;
    private static ImageView captureView;


    private static ImageReader.OnImageAvailableListener readerListener;
    static {
        System.loadLibrary("native-lib");
    }

    public native String classificationFromCaffe2(int h, int w, byte[] Y, byte[] U, byte[] V,
                                                  int rowStride, int pixelStride, boolean r_hwc);

    public native void initCaffe2(AssetManager mgr);


    private class SetUpNeuralNetwork extends AsyncTask<Void, Void, Void> {
        @Override
        protected Void doInBackground(Void[] v) {
            try {
                initCaffe2(mgr);
                predictedClass = "Neural net loaded! Inferring...";
            } catch (Exception e) {
                Log.d(TAG, "Couldn't load neural network.");
            }
            return null;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        //this.requestWindowFeature(Window.FEATURE_NO_TITLE);

        mgr = getResources().getAssets();
        new SetUpNeuralNetwork().execute();

        //View decorView = getWindow().getDecorView();
        //int uiOptions = View.SYSTEM_UI_FLAG_FULLSCREEN;
        //decorView.setSystemUiVisibility(uiOptions);

        setContentView(R.layout.activity_classify_camera);
        yuvBytes = new byte[3][];

        captureView=(ImageView)findViewById(R.id.captured_image);
        textureView = (TextureView) findViewById(R.id.textureView);
        textureView.setSystemUiVisibility(SYSTEM_UI_FLAG_IMMERSIVE);
        capture=(Button)findViewById(R.id.capture_btn);
        mRgbBuffer=new int[(textureView.getWidth() * textureView.getHeight())*3];
        capture.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                getInfernce();
            }
        });

        final GestureDetector gestureDetector = new GestureDetector(this.getApplicationContext(),
                new GestureDetector.SimpleOnGestureListener(){
                    @Override
                    public boolean onDoubleTap(MotionEvent e) {
                        return true;
                    }

                    @Override
                    public void onLongPress(MotionEvent e) {
                        super.onLongPress(e);

                    }

                    @Override
                    public boolean onDoubleTapEvent(MotionEvent e) {
                        return true;
                    }

                    @Override
                    public boolean onDown(MotionEvent e) {
                        return true;
                    }
                });

        textureView.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                return gestureDetector.onTouchEvent(event);
            }
        });

        assert textureView != null;
        textureView.setSurfaceTextureListener(textureListener);
        tv = (TextView) findViewById(R.id.sample_text);

    }

    TextureView.SurfaceTextureListener textureListener = new TextureView.SurfaceTextureListener() {
        @Override
        public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
            //open your camera here
            openCamera();
        }
        @Override
        public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {
            // Transform you image captured size according to the surface width and height
        }
        @Override
        public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) {
            return false;
        }
        @Override
        public void onSurfaceTextureUpdated(SurfaceTexture surface) {
        }
    };
    private final CameraDevice.StateCallback stateCallback = new CameraDevice.StateCallback() {
        @Override
        public void onOpened(CameraDevice camera) {
            cameraDevice = camera;
            createCameraPreview();
        }
        @Override
        public void onDisconnected(CameraDevice camera) {
            cameraDevice.close();
        }
        @Override
        public void onError(CameraDevice camera, int error) {
            cameraDevice.close();
            cameraDevice = null;
        }
    };
    protected void startBackgroundThread() {
        mBackgroundThread = new HandlerThread("Camera Background");
        mBackgroundThread.start();
        mBackgroundHandler = new Handler(mBackgroundThread.getLooper());
    }
    protected void stopBackgroundThread() {
        mBackgroundThread.quitSafely();
        try {
            mBackgroundThread.join();
            mBackgroundThread = null;
            mBackgroundHandler = null;
        } catch (InterruptedException e) {
            e.printStackTrace();
        }
    }

    public void getInfernce() {
        Log.d("harsimarSingh","inferenceStarted");
        predictedClass = classificationFromCaffe2(h, w, Y, U, V,
                rowStride, pixelStride, run_HWC);
        Log.d("harsimarSingh","finished");
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                tv.setText(predictedClass);
            }
        });

    }
    protected void createCameraPreview() {
        rgbBytes = new int[imageDimension.getHeight() * imageDimension.getWidth()];
        try {
            texture  = textureView.getSurfaceTexture();
            assert texture != null;
            texture.setDefaultBufferSize(imageDimension.getWidth(), imageDimension.getHeight());
            Surface surface = new Surface(texture);
            int width = 227;
            int height = 227;
            reader = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 4);
            readerListener = new ImageReader.OnImageAvailableListener() {
                @Override
                public void onImageAvailable(ImageReader reader) {
                    try {

                        image = reader.acquireNextImage();
                        if (processing) {
                            image.close();
                            return;
                        }
                        processing = true;
                        w = image.getWidth();
                        h = image.getHeight();

                        ByteBuffer Ybuffer = image.getPlanes()[0].getBuffer();
                        ByteBuffer Ubuffer = image.getPlanes()[1].getBuffer();
                        ByteBuffer Vbuffer = image.getPlanes()[2].getBuffer();
                        // TODO: use these for proper image processing on different formats.
                        rowStride = image.getPlanes()[1].getRowStride();
                        pixelStride = image.getPlanes()[1].getPixelStride();
                        Y = new byte[Ybuffer.capacity()];
                        U = new byte[Ubuffer.capacity()];
                        V = new byte[Vbuffer.capacity()];
                        Ybuffer.get(Y);
                        Ubuffer.get(U);
                        Vbuffer.get(V);

                        final Image.Plane[] planes = image.getPlanes();
                        Image.Plane[] copyPlanes=planes.clone();

                        yuvBytes[0]=Y;
                        yuvBytes[1]=U;
                        yuvBytes[2]=V;
                        final int yRowStride = planes[0].getRowStride();
                        final int uvRowStride = planes[1].getRowStride();
                        final int uvPixelStride = planes[1].getPixelStride();

                        ConvertYUV420ToARGB8888(
                                yuvBytes[0],
                                yuvBytes[1],
                                yuvBytes[2],
                                rgbBytes,
                                imageDimension.getWidth(),
                                imageDimension.getHeight(),
                                yRowStride,
                                uvRowStride,
                                uvPixelStride,
                                false);

















                        processing = false;
                    } finally {
                        if (image != null) {
                            image.close();
                        }
                    }
                }
            };
            reader.setOnImageAvailableListener(readerListener, mBackgroundHandler);
            captureRequestBuilder = cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            captureRequestBuilder.addTarget(surface);
            captureRequestBuilder.addTarget(reader.getSurface());

            cameraDevice.createCaptureSession(Arrays.asList(surface, reader.getSurface()), new CameraCaptureSession.StateCallback(){
                @Override
                public void onConfigured(@NonNull CameraCaptureSession cameraCaptureSession) {
                    if (null == cameraDevice) {
                        return;
                    }
                    cameraCaptureSessions = cameraCaptureSession;
                    updatePreview();
                }
                @Override
                public void onConfigureFailed(@NonNull CameraCaptureSession cameraCaptureSession) {
                    Toast.makeText(ClassifyCamera.this, "Configuration change", Toast.LENGTH_SHORT).show();
                }
            }, null);
        } catch (CameraAccessException e) {
            e.printStackTrace();
        }
    }

    private void ConvertYUV420ToARGB8888(byte[] yuvByte, byte[] yuvByte1, 
                                         byte[] yuvByte2, int[] rgbBytes, int width,
                                         int height, int yRowStride, int uvRowStride, 
                                         int uvPixelStride, boolean b) {
    }
    
    private void openCamera() {
        CameraManager manager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
        try {
            cameraId = manager.getCameraIdList()[0];
            CameraCharacteristics characteristics = manager.getCameraCharacteristics(cameraId);
            StreamConfigurationMap map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            assert map != null;
            imageDimension = map.getOutputSizes(SurfaceTexture.class)[0];
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED && ActivityCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(ClassifyCamera.this, new String[]{Manifest.permission.CAMERA, Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_CAMERA_PERMISSION);
                return;
            }
            manager.openCamera(cameraId, stateCallback, null);
        } catch (CameraAccessException e) {
            e.printStackTrace();
        }
    }

    protected void updatePreview() {
        if(null == cameraDevice) {
            return;
        }
        captureRequestBuilder.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO);
        try {
            cameraCaptureSessions.setRepeatingRequest(captureRequestBuilder.build(), null, mBackgroundHandler);
        } catch (CameraAccessException e) {
            e.printStackTrace();
        }
    }

    private void closeCamera() {
        if (null != cameraDevice) {
            cameraDevice.close();
            cameraDevice = null;
        }
    }
    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        if (requestCode == REQUEST_CAMERA_PERMISSION) {
            if (grantResults[0] == PackageManager.PERMISSION_DENIED) {
                Toast.makeText(ClassifyCamera.this, "You can't use this app without granting permission", Toast.LENGTH_LONG).show();
                finish();
            }
        }
    }
    @Override
    protected void onResume() {
        super.onResume();
        startBackgroundThread();
        if (textureView.isAvailable()) {
            openCamera();
        } else {
            textureView.setSurfaceTextureListener(textureListener);
        }
    }

    @Override
    protected void onPause() {
        closeCamera();
        stopBackgroundThread();
        super.onPause();
    }
}
