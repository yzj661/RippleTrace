package samples.javaservice;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Sample Java service used to exercise the Java extractor.
 *
 * Includes the constructs that trip a naive parser:
 *   - constructors and overloaded methods
 *   - static and instance methods
 *   - generics and nested generic calls
 *   - a method with a throws clause
 *   - an inner class
 *   - anonymous class and lambda expressions
 */
public class Service {

    private final Map<String, User> users = new HashMap<>();
    private final List<String> audit = new ArrayList<>();

    public Service() {
        this("default");
    }

    public Service(String name) {
        this.name = name;
        audit.add("created:" + name);
    }

    private final String name;

    public String getName() {
        return name;
    }

    public boolean addUser(String id, String email) {
        if (id == null || id.isEmpty()) {
            return false;
        }
        if (users.containsKey(id)) {
            return false;
        }
        users.put(id, new User(id, normalizeEmail(email)));
        audit.add("add:" + id);
        return true;
    }

    public static String normalizeEmail(String email) {
        return email == null ? "" : email.trim().toLowerCase();
    }

    public User find(String id) {
        return users.get(id);
    }

    public int count() {
        return users.size();
    }

    public String summary() {
        StringBuilder builder = new StringBuilder();
        for (User user : users.values()) {
            builder.append(user.describe());
            builder.append(", ");
        }
        return builder.toString();
    }

    public List<String> auditTrail() throws IllegalStateException {
        if (audit.isEmpty()) {
            throw new IllegalStateException("no audit entries");
        }
        return new ArrayList<>(audit);
    }

    public double averageScore() {
        if (users.isEmpty()) {
            return 0.0;
        }
        double total = 0.0;
        for (User user : users.values()) {
            total += user.getScore();
        }
        return total / count();
    }

    public void report() {
        // anonymous class
        Runnable task = new Runnable() {
            @Override
            public void run() {
                System.out.println(summary());
            }
        };
        task.run();

        // lambda
        users.values().forEach(user -> System.out.println(user.describe()));
    }

    /** Inner class: methods here get a dotted qualified name. */
    public static class User {
        private final String id;
        private final String email;
        private int score;

        public User(String id, String email) {
            this.id = id;
            this.email = email;
        }

        public String describe() {
            return id + " <" + email + ">";
        }

        public int getScore() {
            return score;
        }

        public void setScore(int value) {
            this.score = value;
        }
    }

    public static void main(String[] args) {
        Service service = new Service("demo");
        service.addUser("u1", "  Alice@Example.COM ");
        service.addUser("u2", "bob@example.com");
        service.report();
        System.out.println(service.count());
    }
}
