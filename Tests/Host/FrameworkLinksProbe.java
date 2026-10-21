// Host-JVM check of FrameworkLinks (no emulator): Google's in-app install links
// become the store's public page; everything else is untouched.
public final class FrameworkLinksProbe {
    private static int failures;

    private static void expect(String url, String wanted) {
        String got = FrameworkLinks.webLink(url);
        if (!got.equals(wanted)) { System.out.println("FAIL " + url + "\n  got    " + got + "\n  wanted " + wanted); failures++; }
    }

    public static void main(String[] args) {
        String page = "https://play.google.com/store/apps/details?id=";
        // The live ad link (Hungry Shark World): query noise is dropped, the id kept.
        expect("https://play.google.com/d?id=com.aofl.abcmouse&inline=true&enifd=ABT2UUjF9c-dz&lft=1&gclid=EAIa_vD_BwE"
                + "&referrer=gclid%3DEAIa%26gbraid%3D0AAAAA9&gad_source%3D5&gref=EikQ",
                page + "com.aofl.abcmouse");
        expect("http://play.google.com/d?id=com.run.tower.defense", page + "com.run.tower.defense");
        expect("https://PLAY.GOOGLE.COM/d/?inline=true&id=a.b_c9", page + "a.b_c9");
        expect("https://play.google.com/d?id=com.x.y#Intent;package=com.android.vending;end", page + "com.x.y");
        // Already a public page, other hosts and other paths stay as they are.
        expect(page + "com.x.y&hl=en", page + "com.x.y&hl=en");
        expect("https://play.google.com/store/apps/details?id=com.x.y", "https://play.google.com/store/apps/details?id=com.x.y");
        expect("https://example.test/d?id=com.x.y", "https://example.test/d?id=com.x.y");
        expect("https://play.google.com.example.test/d?id=com.x.y", "https://play.google.com.example.test/d?id=com.x.y");
        expect("https://play.google.com/dx?id=com.x.y", "https://play.google.com/dx?id=com.x.y");
        expect("https://oceana.org/", "https://oceana.org/");
        // A /d link without a usable id is left alone, never turned into a guess.
        expect("https://play.google.com/d?inline=true", "https://play.google.com/d?inline=true");
        expect("https://play.google.com/d?id=", "https://play.google.com/d?id=");
        expect("https://play.google.com/d?id=bad/name", "https://play.google.com/d?id=bad/name");
        expect("https://play.google.com/d?id=a b", "https://play.google.com/d?id=a b");
        expect("not a url at all", "not a url at all");
        if (!FrameworkLinks.storePage("com.x.y").equals(page + "com.x.y")) { System.out.println("FAIL storePage"); failures++; }
        System.out.println(failures == 0 ? "FrameworkLinksProbe: ALL PASS" : "FrameworkLinksProbe: " + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
