package uk.nandi.latha;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;

public class LathaActivity extends NativeActivity {
    private static final int REQ_PICK_WASM = 1001;

    private static native void nativeOnPickResult(String uri);

    public void pickWasmFile() {
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                startActivityForResult(intent, REQ_PICK_WASM);
            } catch (Throwable t) {
                Log.e("latha", "pickWasmFile failed", t);
                nativeOnPickResult(null);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_WASM) {
            String uri = null;
            if (resultCode == RESULT_OK && data != null) {
                Uri u = data.getData();
                if (u != null) {
                    uri = u.toString();
                }
            }
            nativeOnPickResult(uri);
        }
    }
}
