// build-apk.ps1's helper (run with `java ExportKey.java`): the signing key of a PKCS12 keystore as PKCS#8 DER plus
// its certificate as X.509 DER, the form Project Gestalt signs the NieR Gestalt app with on the phone (apksig).
import java.io.FileInputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.KeyStore;
import java.security.PrivateKey;

public class ExportKey {
    public static void main(String[] args) throws Exception {
        // keystore, password, alias, output folder
        KeyStore ks = KeyStore.getInstance("PKCS12");
        try (FileInputStream in = new FileInputStream(args[0])) {
            ks.load(in, args[1].toCharArray());
        }
        PrivateKey key = (PrivateKey) ks.getKey(args[2], args[1].toCharArray());
        Path out = Path.of(args[3]);
        Files.createDirectories(out);
        Files.write(out.resolve("key.pk8"), key.getEncoded());
        Files.write(out.resolve("cert.der"), ks.getCertificate(args[2]).getEncoded());
    }
}
