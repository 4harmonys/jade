fn fib(n: f64) -> f64 {
    if n < 3. {
        1.
    } else {
        fib(n - 1.) + fib(n - 2.)
    }
}

fn main() {
    println!("{}", fib(50.));
}
